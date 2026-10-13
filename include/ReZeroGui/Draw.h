#pragma once
#include <cstdint>
#include <cmath>
#include <limits>
#include <ReZeroGui/Allocator.h>
#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {
    using TextureId = std::uintptr_t;

    constexpr TextureId InvalidTexture = std::numeric_limits<TextureId>::max();

    /// non-owning
    struct TextureDescription {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    enum class ResourceCmdType : uint8_t { CreateTexture, UpdateTexture, DestroyTexture };

    // 从 UI 层传递到后端的纹理生命周期请求。
    //
    // 纹理由 UI 上下文分配句柄（绝不由后端分配，也绝不重复使用），
    // 因此后端只需要将纹理句柄映射到对应的设备对象即可。
    // 像素数据采用借用方式传递：在渲染器处理完该命令之前，
    // 像素数据必须始终保持有效。
    struct ResourceCmd {
        ResourceCmdType type = ResourceCmdType::CreateTexture;
        TextureId handle = InvalidTexture;
        TextureDescription desc;
        const std::uint8_t *pixels = nullptr; // RGBA8
    };

    struct textureCommandList {
        explicit textureCommandList(AllocatorView allocator) noexcept : textureList(allocator) {}

        // 分配一个纹理句柄，并请求后端创建该纹理。
        //
        // 这里不会实际创建纹理：创建请求会先进入队列，再由渲染器执行。
        // 渲染器是唯一允许直接操作图形设备的层。
        // 纹理句柄在整个 Context 的生命周期内都是唯一的，并且永不重复使用，
        // 因此，过期的绘制命令绝不会因此采样到其他无关纹理。
        //
        // rgba 采用借用方式传递，因此在下一帧完成渲染之前，
        // 这块像素数据必须始终保持有效。
        auto createTexture(std::uint32_t width, std::uint32_t height, const std::uint8_t *rgba) noexcept -> TextureId {
            if (width == 0 || height == 0) {
                return InvalidTexture;
            }

            ResourceCmd command;
            command.type = ResourceCmdType::CreateTexture;
            command.handle = nextTextureHandle++;
            command.desc.width = width;
            command.desc.height = height;
            command.pixels = rgba;
            if (!textureList.push_back(command)) {
                return InvalidTexture;
            }
            return command.handle;
        }

        /// Queues the release of a texture previously handed out by createTexture.
        /// Destroying an unknown or already destroyed handle is a no-op.
        void destroyTexture(TextureId handle) noexcept {
            if (handle == InvalidTexture) {
                return;
            }
            ResourceCmd command;
            command.type = ResourceCmdType::DestroyTexture;
            command.handle = handle;
            textureList.push_back(command);
        }

        auto span() const noexcept -> Span<const ResourceCmd> { return textureList.span(); }

      private:
        // 单调递增的纹理句柄分配器。
        // 纹理句柄永不重复使用，因此后端可以将创建操作视为幂等操作，
        // 而无需使用任何代数器来区分不同的句柄实例。
        TextureId nextTextureHandle = 1;
        ArrayList<ResourceCmd> textureList;
    };

    enum class DrawCorner : std::uint8_t {
        None = 0,
        TopLeft = 1,
        TopRight = 2,
        BottomLeft = 4,
        BottomRight = 8,
        Top = TopLeft | TopRight,
        Bottom = BottomLeft | BottomRight,
        Left = TopLeft | BottomLeft,
        Right = TopRight | BottomRight,
        All = TopLeft | TopRight | BottomLeft | BottomRight,

    };

    constexpr DrawCorner operator|(DrawCorner lhs, DrawCorner rhs) noexcept {
        return static_cast<DrawCorner>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
    }

    constexpr DrawCorner operator&(DrawCorner lhs, DrawCorner rhs) noexcept {
        return static_cast<DrawCorner>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
    }

    constexpr bool hasCorner(DrawCorner corners, DrawCorner corner) noexcept {
        return (static_cast<std::uint8_t>(corners) & static_cast<std::uint8_t>(corner)) != 0;
    }

    using DrawIndex = std::uint32_t;

    struct DrawVertex {
        Vector2 position;
        Vector2 uv;
        Pixel32 pixel = 0;
    };
    struct DrawCommand {
        Rect clipRect;
        std::uint32_t indexOffset{0};
        std::uint32_t indexCount{0};
        TextureId textureId = InvalidTexture;
    };
    struct DrawList {
        // 1.57079632679 ≈ π / 2 = 90°
        static constexpr float halfPI = 1.57079632679f;
        static constexpr float pi = 3.14159265359f;
        static constexpr float threeHalfPI = 4.71238898038f;
        static constexpr float twoPi = 6.28318530718f;

        // 圆形抗锯齿边缘的宽度，单位为像素。
        // 像素覆盖率存储在顶点的 Alpha 分量中，而现有的混合状态已经会处理该分量，
        // 因此既不需要多重采样（Multisampling），也不需要额外的后处理阶段（Post Pass）。
        static constexpr float featherWidth = 1.0f;

      public:
        explicit DrawList(AllocatorView allocator) noexcept
            : vertices(allocator), indices(allocator), commands(allocator), clipStack(allocator) {}

        DrawList(const DrawList &) = delete;
        DrawList &operator=(const DrawList &) = delete;

        void onFrameBegin() noexcept {
            vertices.clear();
            indices.clear();
            commands.clear();
            clipStack.clear();
        }

        /// 把另一批命令原样追加到末尾。
        ///
        /// 只搬命令，顶点和索引留在原地：每条命令自带它要绘制的索引区间（indexOffset /
        /// indexCount，其中的索引是绝对的顶点引用），而渲染端只按命令数组的顺序遍历
        /// （见 Dx11Impl 的 renderList），所以**命令数组的顺序就是绘制顺序**。
        /// 于是把某个窗口的几何挪到别的层，只需要挪它的命令，搬顶点是白做的。
        void appendCommands(Span<const DrawCommand> source) noexcept {
            for (const DrawCommand &command : source) {
                commands.push_back(command);
            }
        }

        /// 丢掉所有命令，保留顶点和索引。窗口几何重排用它就地重建命令顺序。
        void clearCommands() noexcept { commands.clear(); }
        auto addLine(Vector2 src, Vector2 dest, Pixel pixel, float thickness = 1.0f) noexcept {
            if (pixel.a <= 0.0f) {
                return;
            }
            const Vector2 delta = dest - src;
            thickness = maximum(thickness, 1.0f);
            const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            if (length <= 0.0f) {
                return;
            }

            const Vector2 unit = delta / length;
            const Vector2 normal = Vector2(-unit.y, unit.x) * (thickness * 0.5f);
            addQuadFilled(src + normal, dest + normal, dest - normal, src - normal, pixel);
        }

        auto addRect(Vector2 minimum, Vector2 maximum, Pixel color, float rounding = 0.0f,
                     DrawCorner corners = DrawCorner::All, float thickness = 1.0f) noexcept {
            if (color.a <= 0.0f || thickness <= 0.0f) {
                return;
            }
            addRoundedRectOutline(minimum, maximum, color, rounding, corners, thickness);
        }

        void addRectFilled(Vector2 minimum, Vector2 maximum, Pixel color, float rounding = 0.0f,
                           DrawCorner corners = DrawCorner::All) noexcept {
            if (color.a <= 0.0f) {
                return;
            }

            addRoundedRectFilled(minimum, maximum, color, rounding, corners);
        }

        void addQuadFilled(Vector2 first, Vector2 second, Vector2 third, Vector2 fourth, Pixel color) noexcept {
            if (color.a <= 0.0f) {
                return;
            }
            beginPrimitive(InvalidTexture);
            const std::uint32_t base = vertexCount();
            const Vector2 uv(0.0f, 0.0f);
            addVertex(first, uv, color);
            addVertex(second, uv, color);
            addVertex(third, uv, color);
            addVertex(fourth, uv, color);
            addQuadIndices(base);
        }
        void addTriangleFilled(Vector2 first, Vector2 second, Vector2 third, Pixel color) noexcept {
            if (color.a <= 0.0f) {
                return;
            }
            beginPrimitive(InvalidTexture);
            const std::uint32_t base = vertexCount();
            const Vector2 uv(0.0f, 0.0f);
            addVertex(first, uv, color);
            addVertex(second, uv, color);
            addVertex(third, uv, color);
            indices.push_back(base);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            commands.back().indexCount += 3;
        }
        void addTexturedQuad(Vector2 minimum, Vector2 maximum, Vector2 uvMinimum, Vector2 uvMaximum, Pixel color,
                             TextureId texture) noexcept {
            if (color.a <= 0.0f) {
                return;
            }
            beginPrimitive(texture);
            const std::uint32_t base = vertexCount();
            addVertex(minimum, uvMinimum, color);
            addVertex(Vector2(maximum.x, minimum.y), Vector2(uvMaximum.x, uvMinimum.y), color);
            addVertex(maximum, uvMaximum, color);
            addVertex(Vector2(minimum.x, maximum.y), Vector2(uvMinimum.x, uvMaximum.y), color);
            addQuadIndices(base);
        }
        void addCircle(Vector2 center, float radius, Pixel pixel, int segmentCount = 0,
                       float thickness = 1.0f) noexcept {
            if (pixel.a <= 0.0f || radius <= 0.0f || thickness <= 0.0f) {
                return;
            }
            const int segments = segmentCount > 0 ? segmentCount : circleSegmentCount(radius);
            beginPrimitive(InvalidTexture);

            // 四个圆环，依次为：透明、实心、实心、透明。
            // 对圆环的位置进行限制，避免描边过细或过粗时，圆环彼此交叠、顺序颠倒。
            // 如果不进行限制，极细的圆环可能会在同一像素上重复叠加淡化效果，
            // 导致最终呈现的颜色发白、显得不够鲜明。
            const float half = thickness * 0.5f;
            const float halfFeather = featherWidth * 0.5f;
            const float innerFade = maximum(0.0f, radius - half - halfFeather);
            const float innerSolid = maximum(innerFade, radius - half + halfFeather);
            const float outerSolid = maximum(innerSolid, radius + half - halfFeather);
            const float outerFade = maximum(outerSolid, radius + half + halfFeather);

            addFeatheredRing(center, innerFade, 0.0f, innerSolid, pixel.a, segments, pixel);
            addFeatheredRing(center, innerSolid, pixel.a, outerSolid, pixel.a, segments, pixel);
            addFeatheredRing(center, outerSolid, pixel.a, outerFade, 0.0f, segments, pixel);
        }
        void addCircleFilled(Vector2 center, float radius, Pixel pixel, int segmentCount = 0) noexcept {
            if (pixel.a <= 0.0f || radius <= 0.0f) {
                return;
            }
            const int segments = segmentCount > 0 ? segmentCount : circleSegmentCount(radius);
            beginPrimitive(InvalidTexture);

            // 从圆心到羽化边缘以内的部分保持实心，之后才是逐渐淡出的边缘。
            // 如果圆的半径小于羽化宽度，那么整个圆都由渐变边缘构成，
            // 这正是亚像素大小的圆点应有的视觉效果。
            const float solidRadius = radius - featherWidth * 0.5f;
            const float fadeRadius = radius + featherWidth * 0.5f;
            if (solidRadius > 0.0f) {
                addFeatheredRing(center, 0.0f, pixel.a, solidRadius, pixel.a, segments, pixel);
                addFeatheredRing(center, solidRadius, pixel.a, fadeRadius, 0.0f, segments, pixel);
            } else {
                addFeatheredRing(center, 0.0f, pixel.a, fadeRadius, 0.0f, segments, pixel);
            }
        }

        auto setDisplayRect(Rect rect) noexcept { displayRect = rect; }

        // clipping
        auto pushClipRect(Rect rect) noexcept {
            if (clipStack.empty()) {
                clipStack.push_back(rect.intersected(displayRect));
            } else {
                clipStack.push_back(rect.intersected(clipStack.back()));
            }
        }

        auto popClipRect() noexcept {
            if (!clipStack.empty()) {
                clipStack.pop_back();
            }
        }

        auto currentClipRect() const noexcept -> Rect { return clipStack.empty() ? displayRect : clipStack.back(); }

        auto clipDepth() const noexcept -> std::size_t { return clipStack.size(); }

        Span<const DrawVertex> currentVertices() const noexcept { return vertices.span(); }
        Span<const DrawIndex> currentIndices() const noexcept { return indices.span(); }
        Span<const DrawCommand> currentCommands() const noexcept { return commands.span(); }
        bool isEmpty() const noexcept { return indices.empty(); }
        std::uint32_t vertexCount() const noexcept { return static_cast<std::uint32_t>(vertices.size()); }
        std::uint32_t indexCount() const noexcept { return static_cast<std::uint32_t>(indices.size()); }
        std::uint32_t commandCount() const noexcept { return static_cast<std::uint32_t>(commands.size()); }

      private:
        // 在两个同心圆环之间生成 `segments` 个四边形，并将 Alpha 值从
        // `innerAlpha` 逐渐过渡到 `outerAlpha`。
        // 两个圆环共用相同的角度采样点，确保它们之间的带状区域无缝衔接。
        // 当内圆半径为零时，该带状区域会收缩为一个三角扇形。
        void addFeatheredRing(Vector2 center, float innerRadius, float innerAlpha, float outerRadius, float outerAlpha,
                              int segments, Pixel pixel) {
            const Vector2 uv(0.0f, 0.0f);
            const std::uint32_t base = vertexCount();
            for (size_t index = 0; index < static_cast<size_t>(segments); index += 1) {
                const float angle = (static_cast<float>(index) / static_cast<float>(segments)) * twoPi;
                const Vector2 direction(std::cos(angle), std::sin(angle));
                addVertex(center + direction * innerRadius, uv, pixel.withAlpha(innerAlpha));
                addVertex(center + direction * outerRadius, uv, pixel.withAlpha(outerAlpha));
            }
            for (size_t index = 0; index < static_cast<size_t>(segments); index += 1) {
                const std::uint32_t current = base + static_cast<std::uint32_t>(index) * 2;
                const std::uint32_t next = base + static_cast<std::uint32_t>((index + 1) % segments) * 2;
                indices.push_back(current);
                indices.push_back(next);
                indices.push_back(next + 1);
                indices.push_back(current);
                indices.push_back(next + 1);
                indices.push_back(current + 1);
            }
            commands.back().indexCount += static_cast<std::uint32_t>(segments) * 6;
        }
        void addVertex(Vector2 position, Vector2 uv, Pixel color) {
            vertices.push_back(DrawVertex{position, uv, color.toPixel32()});
        }

        void addQuadIndices(std::uint32_t base) {
            indices.push_back(base);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
            commands.back().indexCount += 6;
        }

        void beginPrimitive(TextureId texture) {
            const Rect clip = currentClipRect();
            if (!commands.empty()) {
                DrawCommand &command = commands.back();
                if (command.indexCount == 0) {
                    command.textureId = texture;
                    command.clipRect = clip;
                    command.indexOffset = static_cast<std::uint32_t>(indices.size());
                    return;
                }
                if (command.textureId == texture && command.clipRect.x == clip.x && command.clipRect.y == clip.y &&
                    command.clipRect.width == clip.width && command.clipRect.height == clip.height) {
                    return;
                }
            }
            DrawCommand command;
            command.textureId = texture;
            command.clipRect = clip;
            command.indexOffset = static_cast<std::uint32_t>(indices.size());
            commands.push_back(command);
        }
        static int circleSegmentCount(float radius) noexcept {
            constexpr float error = 0.25f;

            if (radius <= 1.0f) {
                return 8;
            }

            const float angle = std::acos(1.0f - error / radius);
            const int count = static_cast<int>(std::ceil(3.14159265359f / angle));

            // clampFloat is a float helper; spelling the conversion out keeps /W4 quiet about
            // the int it would otherwise convert twice.
            return static_cast<int>(clampFloat(static_cast<float>(count), 8.0f, 64.0f));
        }

        // 每个 90 度圆角所使用的线段数：确保近似圆弧与真实圆弧之间的偏差
        // 小于约 0.2 个像素，同时设置上限，避免圆角半径过大时生成过多顶点。
        auto roundedRectSegments(float radius) noexcept -> int {
            if (radius <= 0.0f) {
                return 2;
            }
            const float ratio = 1.0f - 0.2f / radius;
            if (ratio <= -1.0f) {
                return 2;
            }
            const int count = static_cast<int>(halfPI / std::acos(ratio)) + 1;
            return count < 2 ? 2 : (count > 32 ? 32 : count);
        }
        auto clampRoundedRectRadius(Vector2 minimum, Vector2 maximum, float rounding) noexcept -> float {
            float radius = rounding < 0.0f ? 0.0f : rounding;
            const float halfWidth = (maximum.x - minimum.x) * 0.5f;
            const float halfHeight = (maximum.y - minimum.y) * 0.5f;
            if (radius > halfWidth) {
                radius = halfWidth;
            }
            if (radius > halfHeight) {
                radius = halfHeight;
            }
            return radius < 0.0f ? 0.0f : radius;
        }

        /// One point of a rounded rectangle outline, plus the direction that points out
        /// of the shape there.
        struct PathPoint {
            Vector2 position;
            Vector2 normal;
        };

        // 按顺时针方向遍历轮廓，将每个点及其对应的外法线传递给 `emit`。
        // 相邻圆弧之间的间隙构成直线边；间隙两端的法线相同，因此无需特殊处理。
        // 对于未进行圆角处理的尖角，会针对相邻的两条边分别输出一次该尖角，
        // 每次使用对应边的法线；这两个尖角顶点之间的间隙会形成羽化带的斜接三角形（Miter Triangle）。
        template <class Emit>
        static void walkRoundedRect(Vector2 minimum, Vector2 maximum, float radius, DrawCorner corners, int segments,
                                    Emit &&emit) {
            constexpr float QuarterTurn = halfPI;
            const Vector2 centers[4] = {
                Vector2(maximum.x - radius, minimum.y + radius), // top right
                Vector2(maximum.x - radius, maximum.y - radius), // bottom right
                Vector2(minimum.x + radius, maximum.y - radius), // bottom left
                Vector2(minimum.x + radius, minimum.y + radius), // top left
            };
            const float startAngles[4] = {-QuarterTurn, 0.0f, QuarterTurn, 2.0f * QuarterTurn};
            const DrawCorner masks[4] = {DrawCorner::TopRight, DrawCorner::BottomRight, DrawCorner::BottomLeft,
                                         DrawCorner::TopLeft};

            for (size_t corner = 0; corner < 4; corner += 1) {
                const Vector2 center = centers[corner];
                const float start = startAngles[corner];
                if (hasCorner(corners, masks[corner])) {
                    for (size_t step = 0; step <= static_cast<size_t>(segments); step += 1) {
                        const float sweep = QuarterTurn * static_cast<float>(step) / static_cast<float>(segments);
                        const Vector2 direction(std::cos(start + sweep), std::sin(start + sweep));
                        emit(PathPoint{center + direction * radius, direction});
                    }
                } else {
                    const Vector2 incoming(std::cos(start), std::sin(start));
                    const Vector2 outgoing(std::cos(start + QuarterTurn), std::sin(start + QuarterTurn));
                    const Vector2 sharp = center + incoming * radius + outgoing * radius;
                    emit(PathPoint{sharp, incoming});
                    emit(PathPoint{sharp, outgoing});
                }
            }
        }

        void addRoundedRectFilled(Vector2 minimum, Vector2 maximum, Pixel color, float rounding, DrawCorner corners) {
            if (maximum.x <= minimum.x || maximum.y <= minimum.y) {
                return;
            }
            const float radius = clampRoundedRectRadius(minimum, maximum, rounding);
            const int segments = roundedRectSegments(radius);
            const Vector2 uv(0.0f, 0.0f);

            beginPrimitive(InvalidTexture);
            const std::uint32_t center = vertexCount();
            addVertex(Vector2((minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f), uv, color);

            std::uint32_t firstInner = 0;
            std::uint32_t firstOuter = 0;
            std::uint32_t previousInner = 0;
            std::uint32_t previousOuter = 0;
            int pointCount = 0;
            walkRoundedRect(minimum, maximum, radius, corners, segments, [&](const PathPoint &point) {
                const std::uint32_t inner = vertexCount();
                addVertex(point.position, uv, color);
                const std::uint32_t outer = vertexCount();
                addVertex(point.position + point.normal * featherWidth, uv, color.withAlpha(0.0f));

                if (pointCount == 0) {
                    firstInner = inner;
                    firstOuter = outer;
                } else {
                    // A triangle of the interior fan, then the two of the fading band.
                    indices.push_back(center);
                    indices.push_back(previousInner);
                    indices.push_back(inner);
                    indices.push_back(previousInner);
                    indices.push_back(inner);
                    indices.push_back(outer);
                    indices.push_back(previousInner);
                    indices.push_back(outer);
                    indices.push_back(previousOuter);
                    commands.back().indexCount += 9;
                }

                previousInner = inner;
                previousOuter = outer;
                pointCount += 1;
            });

            if (pointCount > 1) {
                indices.push_back(center);
                indices.push_back(previousInner);
                indices.push_back(firstInner);
                indices.push_back(previousInner);
                indices.push_back(firstInner);
                indices.push_back(firstOuter);
                indices.push_back(previousInner);
                indices.push_back(firstOuter);
                indices.push_back(previousOuter);
                commands.back().indexCount += 9;
            }
        }

        // 沿着相同的路径绘制边框，保持原有的描边带宽度不变：
        // 从路径向内延伸 `thickness` 的距离，并在描边带的内外两侧各用一个像素进行渐变。
        // 如果描边厚度超过其所沿形状的尺寸，描边就会发生自我重叠，
        // 这与普通描边折线的行为完全相同。
        //
        // 四层顶点构成三个带状区域：
        //
        //        外侧                          内侧
        //         ↓                             ↓
        //
        //    A0 ============================ B0    alpha = 0
        //     │                              │
        //     │     透明 / 羽化区域           │    0 → 1
        //     │                              │
        //    A1 ============================ B1    alpha = 1
        //     │                              │
        //     │          实心描边             │    1 → 1
        //     │                              │
        //    A2 ============================ B2    alpha = 1
        //     │                              │
        //     │     透明 / 羽化区域           │    1 → 0
        //     │                              │
        //    A3 ============================ B3    alpha = 0
        //
        //         ←────── 描边宽度 ──────→
        //
        // A0-A1：外侧羽化区域
        // A1-A2：实心描边区域
        // A2-A3：内侧羽化区域
        //
        // 每一对相邻的圆环层之间，都通过三角形连接。
        void addRoundedRectOutline(Vector2 minimum, Vector2 maximum, Pixel color, float rounding, DrawCorner corners,
                                   float thickness) {

            if (maximum.x <= minimum.x || maximum.y <= minimum.y || thickness <= 0.0f) {
                return;
            }

            const float radius = clampRoundedRectRadius(minimum, maximum, rounding);
            const int segments = roundedRectSegments(radius);
            const Vector2 uv(0.0f, 0.0f);

            // Four offsets across the border: fading in, solid, solid, fading out.
            const float offsets[4] = {-thickness - featherWidth, -thickness, 0.0f, featherWidth};
            const float alphas[4] = {0.0f, color.a, color.a, 0.0f};

            beginPrimitive(InvalidTexture);
            std::uint32_t previous[4] = {0, 0, 0, 0};
            std::uint32_t first[4] = {0, 0, 0, 0};
            int pointCount = 0;

            walkRoundedRect(minimum, maximum, radius, corners, segments, [&](const PathPoint &point) {
                std::uint32_t ring[4] = {0, 0, 0, 0};
                for (size_t band = 0; band < 4; band += 1) {
                    ring[band] = vertexCount();
                    addVertex(point.position + point.normal * offsets[band], uv, color.withAlpha(alphas[band]));
                }

                if (pointCount == 0) {
                    for (int band = 0; band < 4; ++band) {
                        first[band] = ring[band];
                    }
                } else {
                    for (size_t band = 0; band + 1 < 4; band += 1) {
                        indices.push_back(previous[band]);
                        indices.push_back(ring[band]);
                        indices.push_back(ring[band + 1]);
                        indices.push_back(previous[band]);
                        indices.push_back(ring[band + 1]);
                        indices.push_back(previous[band + 1]);
                    }
                    commands.back().indexCount += 18;
                }

                for (int band = 0; band < 4; ++band) {
                    previous[band] = ring[band];
                }
                pointCount += 1;
            });

            if (pointCount > 1) {
                for (int band = 0; band + 1 < 4; ++band) {
                    indices.push_back(previous[band]);
                    indices.push_back(first[band]);
                    indices.push_back(first[band + 1]);
                    indices.push_back(previous[band]);
                    indices.push_back(first[band + 1]);
                    indices.push_back(previous[band + 1]);
                }
                commands.back().indexCount += 18;
            }
        }

      private:
        ArrayList<DrawVertex> vertices;
        ArrayList<DrawIndex> indices;
        ArrayList<DrawCommand> commands;
        ArrayList<Rect> clipStack;

        Rect displayRect;
    };

    struct DrawData {
        const DrawList *background = nullptr;
        const DrawList *foreground = nullptr;
        // 最后绘制，覆盖所有窗口：这是弹出窗口所在的绘制层。
        // 弹出窗口必须能够遮盖本帧中在打开它的控件之后声明的其他控件，
        // 因此不能与这些控件共用同一个绘制列表。
        // 此外，窗口的几何数据会在帧结束时按照窗口的堆叠顺序重新构建，
        // 因此这里需要使用独立的绘制列表，而不能将弹出窗口放在前景绘制列表中的某个角落。
        const DrawList *overlay = nullptr;
        // 由 UI 上下文管理的纹理生命周期请求。
        // 后端会在绘制之前，按照请求的顺序依次执行这些请求。
        Span<const ResourceCmd> resourceCommands;
        Vector2 displaySize;
        bool valid = false;
    };
} // namespace ReZeroGui
