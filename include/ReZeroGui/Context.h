#pragma once
#include <ReZeroGui/Allocator.h>
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Id.h>
#include <ReZeroGui/Draw.h>
#include <ReZeroGui/Font.h>
#include <ReZeroGui/Input.h>
#include <ReZeroGui/Layout.h>
#include <ReZeroGui/Renderer.h>
#include <ReZeroGui/Style.h>
#include <ReZeroGui/State.h>
#include <vector>

namespace ReZeroGui {

    /// What a click-style interaction produced this frame. The behaviours below return one
    /// of these instead of writing through out-parameters: a caller names the part it
    /// wants and says nothing about the parts it ignores.
    struct InteractionResult {
        /// The pointer is over the item and the item accepts input.
        bool hovered = false;
        /// The pointer button went down on the item and has not come back up.
        bool held = false;
        /// Pressed and released on the item: the click a button acts on.
        bool clicked = false;
    };

    /// What dragging a value produced this frame.
    struct DragResult {
        /// The pointer is over the item and the item accepts input. A drag that has
        /// wandered outside the item keeps `changed` but loses `hovered`.
        bool hovered = false;
        /// The drag moved the value this frame, i.e. the item is being dragged.
        bool changed = false;
        /// The value after this frame's interaction: what the drag computed while the
        /// button is down, otherwise the value that was passed in. Always worth reading.
        float value = 0.0f;
    };
    /// What a key bind item produced this frame. The binding itself stays with the host, so
    /// the item never writes through a pointer of its own.
    struct KeyBindResult {
        /// What the click behaviour made of this frame's input, for hover and press feedback.
        InteractionResult interaction;
        /// Listening right now: the next key the user presses becomes the binding.
        bool listening = false;
        /// The binding changed this frame; `key` holds the new value.
        bool changed = false;
        /// The binding after this frame. `Input::KeyCode::None` means unbound.
        Input::KeyCode key = Input::KeyCode::None;
    };

    enum class ArrowDirection : std::uint8_t {
        Right,
        Down,
    };
    // WindowScope::position() 设置的窗口位置采用何种应用方式。
    //
    // 控件代码会在每一帧执行。如果每一帧都重新设置窗口位置，
    // 就会阻止用户移动窗口。
    enum class PositionMode : std::uint8_t {
        // 在窗口首次使用时设置位置。
        // 此后保留用户调整后的位置。
        FirstUseEver,

        // 仅设置一次位置，之后不再应用该位置。
        // 用于将窗口移动到新的位置。
        Once,

        // 每一帧都设置位置。
        // 用于必须保持在固定位置的窗口。
        Always,
    };
    inline constexpr std::size_t NoWindow = static_cast<std::size_t>(-1);
    struct WindowParams {
        u8StringView title;
        Vector2 position{0.0f, 0.0f};
        /// Negative means "not specified": the axis then keeps its current size
        /// (windowMinimumSize first, shrink wrapped to the contents afterwards).
        Vector2 size{-1.0f, -1.0f};
        bool hasPosition = false;
        PositionMode positionMode = PositionMode::FirstUseEver;
        Vector2 padding{-1.0f, -1.0f};
        bool hasWidth = false;
        bool hasHeight = false;
        bool hasTitleBar = true;
        bool movable = true;
        bool drawBackground = true;
        bool hasCollapsed = false;
        bool collapsed = false;
        const bool *openState = nullptr;
    };
    struct WindowDescription {
        Id id = 0;
        Rect rect;
        Vector2 padding;
        Vector2 fixedSize{-1.0f, -1.0f};
        bool hasTitleBar = true;
        bool movable = true;
        bool drawBackground = true;
        bool collapsed = false;
        bool skipItems = false;
        /// Set once an explicit position has been applied; used by PositionMode.
        bool positionApplied = false;
        /// Last frame this window was drawn in. Input asks this to know which windows the
        /// user is actually looking at when deciding who is covered by whom.
        std::uint32_t drawnFrame = 0;
        std::size_t clipDepth = 0;
    };

    // 一个窗口在一帧里的登记：它在 `Windows` 里的下标、它的 id，以及它占用的命令区间。
    //
    // 一帧里每个被声明的窗口都有一条，按声明顺序排列，并且**保留到帧末**：帧结束时要按堆叠
    // 顺序重排这些命令区间。登记表只追加、不弹出，所以"最后一条"就是此刻唯一还打开着的那条
    // （窗口不允许嵌套，见 beginWindow()）。
    //
    // 只记命令范围，不记顶点范围：渲染端只按命令数组的顺序绘制，每条命令自带它的索引区间，
    // 所以绘制顺序完全由命令顺序决定，顶点和索引不需要跟着搬。
    struct WindowFrame {
        Id id = 0;
        std::size_t windowIndex = NoWindow;
        std::uint32_t commandBegin = 0;
        std::uint32_t commandEnd = 0;
        /// 这一帧 beginWindow() 是否真的压入了布局区域。布局栈满时压不进去，endWindow() 就不能
        /// 跟着弹——否则弹掉的是外层宿主压入的那一层，之后整帧的区域都对不上。
        bool pushedLayout = false;
    };
    struct Context {
        Context(AllocatorView allocator) noexcept
            : Windows(allocator), WindowOrder(allocator), WindowFrames(allocator), BackgroundList(allocator),
              ForegroundList(allocator), OverlayList(allocator), reorderScratch(allocator), textureList(allocator),
              Atlas(allocator), Arena(allocator) {}
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;
        auto &&displaySize(this auto &&self, Vector2 size) noexcept {
            self.DisplaySize = size;
            return std::forward<decltype(self)>(self);
        }
        auto displaySize() const noexcept -> Vector2 { return DisplaySize; }

        auto io() noexcept -> Input & { return Input; }

        bool wantsKeyboardCapture() const noexcept { return keyboardCapture; }
        bool wantsMouseCapture() const noexcept {
            if (state.interaction.active != InvalidId) {
                return true;
            }
            // hoveredWindowId 是在帧首按"上一帧的布局"定的（那时这一帧的窗口还没声明），所以这里
            // 再确认一次这个窗口本帧真的画了：只声明在上一帧、这一帧消失的窗口不该继续占着光标。
            const WindowDescription *window = findWindow(hoveredWindowId);
            return window != nullptr && window->drawnFrame == FrameIndex;
        }
        /// A widget that owns the keyboard (a focused text field, or a key bind waiting for a key)
        /// calls this; the host reads the result through wantsKeyboardCapture().
        void captureKeyboard(bool capture = true) noexcept { keyboardCapture = capture; }

        /// Sets the window position based on PositionMode.
        /// Prevents repeated position changes.
        // FirstUseEver applies the position only once.
        // Once can apply it again later.
        void applyWindowPosition(WindowDescription &window, const WindowParams &params) noexcept {
            if (!params.hasPosition) {
                return;
            }
            if (params.positionMode == PositionMode::Always) {
                window.rect.x = params.position.x;
                window.rect.y = params.position.y;
                return;
            }
            if (window.positionApplied) {
                return;
            }
            window.rect.x = params.position.x;
            window.rect.y = params.position.y;
            // 两种"只应用一次"的模式（FirstUseEver / Once）到这里都算应用过了，否则 Once 会每帧
            // 重新应用，表现得和 Always 一样：用户刚拖走窗口，松手就被弹回原位。
            window.positionApplied = true;
        }
        /// 新窗口第一帧的临时矩形。
        ///
        /// 窗口的尺寸要到 endWindow() 按内容收缩之后才确定，所以它第一次被声明的那一帧
        /// 必须有一个"能画"的初始矩形：裁剪矩形一旦退化成 0，那一帧的 chrome 和内容就都
        /// 画不出来（isItemVisible() 对零面积裁剪恒为 false）。
        ///
        /// 它和 Style::windowMinimumSize 是两个不同的东西，只是以前共用了一个值：
        /// 那个是最终尺寸的下限（可以为 0，表示紧贴内容），而这个纯粹是第一帧的猜值。
        /// 猜错的代价只有第一帧的尺寸会跳一下，从第二帧起就准了。
        static constexpr Vector2 InitialWindowSize{160.0f, 64.0f};

        auto findOrCreateWindow(Id id) noexcept -> std::size_t {
            for (std::size_t index = 0; index < Windows.size(); ++index) {
                if (Windows[index].id == id) {
                    return index;
                }
            }
            WindowDescription window;
            window.id = id;
            window.rect = Rect(Vector2(32.0f, 32.0f), InitialWindowSize);
            // 分配失败时不能继续往下走：那样 Windows.size() 还是 0，返回 size()-1 会下溢成
            // SIZE_MAX，调用方拿它当下标就是越界访问。
            if (!Windows.push_back(window)) {
                return NoWindow;
            }
            // 一个从未被点击过的窗口，初始时位于窗口栈的最底层；在它之后声明的窗口会绘制在它上面。
            if (!WindowOrder.push_back(id)) {
                Windows.pop_back();
                return NoWindow;
            }
            return Windows.size() - 1;
        }

        auto findWindow(Id id) noexcept -> WindowDescription * {
            for (WindowDescription &window : Windows) {
                if (window.id == id) {
                    return &window;
                }
            }
            return nullptr;
        }

        auto findWindow(Id id) const noexcept -> const WindowDescription * {
            for (const WindowDescription &window : Windows) {
                if (window.id == id) {
                    return &window;
                }
            }
            return nullptr;
        }

        // 如果绘制在 \p id 对应窗口上方的某个窗口覆盖了 \p point，则返回 true。
        // 此时，\p id 对应的窗口不会将该位置的鼠标光标视为位于自身上方。
        // 如果某个窗口是在当前帧或上一帧中绘制的，则认为它仍然具有遮挡作用：
        // 输入事件的判定应以用户最近实际看到的画面为依据。
        bool windowIsCovered(Id id, Vector2 point) const noexcept {
            for (std::size_t index = WindowOrder.size(); index > 0; --index) {
                const Id above = WindowOrder[index - 1];
                if (above == id) {
                    return false; // reached the window itself: nothing above it covers the point
                }
                const WindowDescription *window = findWindow(above);
                if (window != nullptr && window->drawnFrame + 1 >= FrameIndex && window->rect.contains(point)) {
                    return true;
                }
            }
            return false;
        }

        /// Moves \p id to the top of the stacking order.
        void bringWindowToFront(Id id) noexcept {
            for (std::size_t index = 0; index < WindowOrder.size(); ++index) {
                if (WindowOrder[index] != id) {
                    continue;
                }
                for (std::size_t move = index; move + 1 < WindowOrder.size(); ++move) {
                    WindowOrder[move] = WindowOrder[move + 1];
                }
                WindowOrder.back() = id;
                return;
            }
        }

        auto renderArrow(Vector2 center, ArrowDirection direction, Pixel color, float size) noexcept -> void {
            if (direction == ArrowDirection::Down) {
                ForegroundList.addTriangleFilled(Vector2(center.x - size, center.y - size * 0.5f),
                                                 Vector2(center.x + size, center.y - size * 0.5f),
                                                 Vector2(center.x, center.y + size * 0.5f), color);
            } else {
                ForegroundList.addTriangleFilled(Vector2(center.x - size * 0.5f, center.y - size),
                                                 Vector2(center.x - size * 0.5f, center.y + size),
                                                 Vector2(center.x + size * 0.5f, center.y), color);
            }
        }
        bool beginWindow(const WindowParams &params) noexcept {
            if (params.openState != nullptr && !*params.openState) {
                return false;
            }

            // 窗口不能嵌套。重排按"声明顺序的槽位"搬运命令区间，一旦某个窗口的区间落在另一个
            // 窗口内部，这套算术就不成立了（父窗口的区间会整个盖住子窗口，槽位对不上，几何会被
            // 重复画或漏掉）。与其渲染出错，不如直接拒绝：调试下有断言，release 下就是这次声明
            // 被跳过，窗口不显示。
            if (WindowDepth != 0) {
                REZERO_ASSERT_MSG(false, "a window cannot be declared inside another window");
                return false;
            }

            // 窗口身份只取标题的 "##" 之后那半（没有 "##" 时就是整个标题），所以改标题的显示
            // 文本不会把窗口变成一个"新窗口"（位置、尺寸、折叠状态都保得住）。见 Id.h。
            //
            // 用 id() 而不是直接 hashString()：窗口的身份就是"它的标题在最外层作用域里的 id"。
            // 这样在窗口外调用 ctx.id(title) 得到的就是这个窗口的 id（titlebar_test 之类的
            // 宿主代码会这么用），而且和控件 id 用同一套混合方式，不会因为空标题退化成种子本身。
            const std::size_t index = findOrCreateWindow(id(labelId(params.title)));
            if (index == NoWindow) {
                // 内存耗尽（或者是窗口表本身建不出来）。这里必须挡下来，否则下面会拿 NoWindow
                // 当数组下标。
                REZERO_ASSERT_MSG(false, "beginWindow: no room for another window");
                return false;
            }
            WindowDescription &window = Windows[index];

            // 同一个窗口一帧只能声明一次。声明两次时登记表里会有两份 run，而帧末重排只认第一份
            // （findRun() 返回第一个匹配），第二份的命令留在原位：chrome 被画两遍，堆叠位置也不对。
            // 和嵌套一样，调试下断言，release 下这次多余的声明被跳过。
            if (findRun(window.id) != nullptr) {
                REZERO_ASSERT_MSG(false, "the same window cannot be declared twice in one frame");
                return false;
            }

            applyWindowPosition(window, params);
            window.fixedSize = params.size;
            if (params.hasWidth) {
                window.rect.width = params.size.x;
            }
            if (params.hasHeight) {
                window.rect.height = params.size.y;
            }

            // 限制窗口位置，防止窗口完全跑出屏幕,至少保留32像素
            window.rect.x = clampFloat(window.rect.x, 32.0f - window.rect.width, maximum(0.0f, DisplaySize.x - 32.0f));
            window.rect.y = clampFloat(window.rect.y, 0.0f, maximum(0.0f, DisplaySize.y - 32.0f));

            window.padding = params.padding.x < 0.0f ? Style.windowPadding : params.padding;

            window.hasTitleBar = params.hasTitleBar;
            window.movable = params.movable;
            window.drawBackground = params.drawBackground;
            if (!window.hasTitleBar) {
                // 没有标题栏的窗口不允许折叠。
                //
                // 折叠后的高度就是标题栏高度，而没有标题栏时它是 0，窗口会整个消失；而且此时
                // 既没有折叠箭头、也没有标题栏可拖，宿主只能自己把 collapsed 翻回来。
                // 直接忽略这个请求，窗口保持可见。
                window.collapsed = false;
            } else if (params.hasCollapsed) {
                window.collapsed = params.collapsed;
            }

            WindowGeometry geometry = windowGeometry(window);

            if (window.hasTitleBar && window.movable) {
                const Id moveId = hashCombine(window.id, Id(0x9E3779B9u));
                // 用户按住标题栏拖动窗口时，窗口跟着鼠标移动；但如果用户按的是折叠按钮，就不能把这次操作误判成拖动。
                // overHandle：鼠标是否位于折叠按钮上
                const bool overHandle =
                    geometry.hasCollapseHandle && geometry.collapseTriangle.contains(Input.currentMousePosition());
                // 这个窗口本帧还没登记进 WindowFrames，isMouseHoveringRect() 里的"被遮挡"判断因此
                // 走不到，得自己问一次：否则声明顺序在前的、被盖住的那个窗口会先抢到鼠标，用户抓着
                // 上面那个窗口拖，动的却是下面那个。
                const bool covered = windowIsCovered(window.id, Input.currentMousePosition());
                // 激活拖动并记录偏移量
                if (!overHandle && !covered && isMouseHoveringRect(geometry.titleBar, moveId) &&
                    Input.isMouseClicked(Input::MouseButton::Left)) {
                    claimMouse(moveId);
                    WindowDragOffset = Input.currentMousePosition() - window.rect.position();
                }
                if (state.interaction.active == moveId) {
                    holdMouse(moveId);
                    // 按住鼠标时更新窗口位置
                    if (Input.isMouseDown(Input::MouseButton::Left)) {
                        window.rect.x = Input.currentMousePosition().x - WindowDragOffset.x;
                        window.rect.y = Input.currentMousePosition().y - WindowDragOffset.y;
                        // 限制窗口位置，防止窗口完全跑出屏幕
                        window.rect.x =
                            clampFloat(window.rect.x, 32.0f - window.rect.width, maximum(0.0f, DisplaySize.x - 32.0f));
                        window.rect.y = clampFloat(window.rect.y, 0.0f, maximum(0.0f, DisplaySize.y - 32.0f));
                        // 窗口位置发生变化后,必须重新计算几何信息
                        geometry = windowGeometry(window);
                    } else {
                        // 鼠标松开后结束拖动
                        releaseMouse();
                    }
                }
            }

            // 此窗口绘制的所有内容——包括窗口装饰，以及调用方声明控件后生成的窗口内容——
            // 都会被收集到命令流中的一段，供帧结束按堆叠顺序重排。
            window.drawnFrame = FrameIndex;
            const std::uint32_t commandBegin = ForegroundList.commandCount();

            ForegroundList.pushClipRect(geometry.body);
            window.clipDepth = ForegroundList.clipDepth();

            // 先确定窗口是否折叠，再绘制窗口；
            // 如果点击折叠箭头导致状态改变，就立即重新计算几何信息和裁剪区域，确保同一帧中的背景、边框、箭头和内容使用一致的窗口尺寸
            if (geometry.hasCollapseHandle && clickBehavior(geometry.collapseTriangle, geometry.collapseId).clicked) {
                window.collapsed = !window.collapsed;
                // 窗口折叠状态发生变化后,重新计算几何信息和裁剪区域
                geometry = windowGeometry(window);
                ForegroundList.popClipRect();
                ForegroundList.pushClipRect(geometry.body);
                window.clipDepth = ForegroundList.clipDepth();
            }

            if (window.drawBackground) {
                const bool active = hoveredWindowId == window.id;
                ForegroundList.addRectFilled(geometry.body.position(), geometry.body.maxPoint(),
                                             Style.pixel(PixelIndex::WindowBackground), Style.windowRounding);
                if (window.hasTitleBar) {
                    Pixel titleColor = Style.pixel(PixelIndex::TitleBackground);
                    if (window.collapsed) {
                        titleColor = Style.pixel(PixelIndex::TitleBackgroundCollapsed);
                    } else if (active) {
                        titleColor = Style.pixel(PixelIndex::TitleBackgroundActive);
                    }
                    ForegroundList.addRectFilled(geometry.titleBar.position(), geometry.titleBar.maxPoint(), titleColor,
                                                 Style.windowRounding, DrawCorner::Top);

                    // The arrow is a mark drawn inside its box, not the box itself.
                    renderArrow(geometry.collapseTriangle.center(),
                                window.collapsed ? ArrowDirection::Right : ArrowDirection::Down,
                                Style.pixel(PixelIndex::Text), geometry.collapseTriangle.width * 0.4f);

                    // 标题栏画的是显示文本：身份用的那半（"##" 之后）到这里已经被切掉。
                    const u8StringView title(labelText(params.title));
                    const Vector2 titleSize = calculateTextSize(title);
                    // 标题从箭头后面开始，并根据样式设定的对齐方式，在标题栏剩余的空间内向左或向右调整位置。
                    const float available = geometry.titleBar.width - geometry.collapseTriangle.width -
                                            Style.framePadding.x * 2.0f - Style.itemInnerSpacing.x;
                    const float titleX = geometry.titleBar.x + Style.framePadding.x + geometry.collapseTriangle.width +
                                         Style.itemInnerSpacing.x +
                                         maximum(0.0f, (available - titleSize.x) * Style.windowTitleAlignX);
                    const float titleY = geometry.titleBar.y + Style.framePadding.y;
                    renderText(Vector2(titleX, titleY), title, Style.pixel(PixelIndex::Text), font(), 0.0f);
                }
                if (Style.windowBorderSize > 0.0f) {
                    ForegroundList.addRect(geometry.body.position(), geometry.body.maxPoint(),
                                           Style.pixel(PixelIndex::WindowBorder), Style.windowRounding, DrawCorner::All,
                                           Style.windowBorderSize);
                }
            }

            // 从这里开始，窗口的内容处理即将开始。因此，这里是折叠窗口停止显示其内容、停止响应内容区域内鼠标输入的唯一位置。
            // 这段逻辑必须放在前面的窗口外观处理之后：因为前面已经确定的折叠操作更新了窗口的几何信息。这样安排，也能确保这个标志在每一帧中恰好写入一次。
            //
            //  注意，这个标志是通过 currentWindowIndex() 读取的。在 beginWindow()
            //  执行期间，它所指向的仍然是外层窗口——正在开始处理的窗口，要到这个函数结束时才会建立自己的作用域。
            // 因此，这个标志会控制当前窗口在本帧剩余时间里的内容处理；如果一个窗口嵌套在已折叠的窗口中，它也会随之保持不可交互的状态。
            window.skipItems = geometry.skipItems;

            const bool pushedLayout = pushLayoutArea(geometry.contentMin, geometry.contentMax);
            // 到这里这个窗口才算"打开"：登记它，并把作用域深度加一。登记要留到帧末（重排要用），
            // 而深度只表示此刻打开了几个窗口。
            WindowFrames.push_back({.id = window.id,
                                    .windowIndex = index,
                                    .commandBegin = commandBegin,
                                    .commandEnd = 0,
                                    .pushedLayout = pushedLayout});
            WindowDepth += 1;
            return true;
        }

        void endWindow() noexcept {
            if (WindowDepth == 0) {
                // endWindow() 没有配对的 beginWindow()。断言在 NDEBUG 下会消失，所以这里必须
                // 自己挡一下：currentWindowIndex() 会返回 NoWindow，而 Windows[NoWindow] 在
                // ArrayList::operator[] 里是没有边界检查的越界访问。
                REZERO_ASSERT_MSG(false, "endWindow() without a matching beginWindow()");
                return;
            }

            // 当前窗口就是最后一条登记。这条依赖 beginWindow() 里那条"不许嵌套"的断言：
            // 登记表只追加、不弹出（帧末重排还要读它），所以"最后一条"等价于"唯一还打开着的那条"。
            // 注意不能用 WindowFrames[WindowDepth - 1]：WindowDepth 是"打开了几个"（0 或 1），
            // 不是下标——登记表不弹出，条目会一直累积。
            WindowFrame &frame = WindowFrames.back();
            WindowDescription &window = Windows[frame.windowIndex];
            // 重新计算，而不是直接使用之前保存的值：
            // 窗口可能在其内容执行期间被拖动或折叠，
            // 因此这里的几何信息必须与最终确定的窗口矩形保持一致。
            const float titleBarHeight = windowGeometry(window).titleBarHeight;

            const Vector2 contentSize = frame.pushedLayout && hasLayout() ? currentLayoutContext().contentSize()
                                                                         : Vector2();
            if (frame.pushedLayout) {
                // 只有本窗口真的压入过，才弹这一层；否则弹掉的是外层宿主的区域（栈满时就是这样）。
                popLayoutArea();
            }

            if (window.collapsed) {
                window.rect.height = titleBarHeight;
            } else {
                if (window.fixedSize.x < 0.0f) {
                    window.rect.width = maximum(Style.windowMinimumSize.x, contentSize.x + window.padding.x * 2.0f);
                }
                if (window.fixedSize.y < 0.0f) {
                    window.rect.height =
                        maximum(Style.windowMinimumSize.y, contentSize.y + window.padding.y * 2.0f + titleBarHeight);
                }
            }
            REZERO_ASSERT(ForegroundList.clipDepth() == window.clipDepth);
            ForegroundList.popClipRect();

            frame.commandEnd = ForegroundList.commandCount();
            WindowDepth -= 1;
        }

        // 一帧之内允许打开的布局区域的最大嵌套深度。窗口主体本身就占用一层；宿主代码在窗口外部额外压入的布局区域，还会进一步增加嵌套深度。
        // 布局栈使用固定大小的数组，因此每帧布局过程中都不需要动态分配内存，而且即使继续压入新的区域，之前通过
        // currentLayoutContext() 获取的 LayoutContext 引用也仍然有效。
        static constexpr std::size_t MaxLayoutDepth = 16;
        bool hasLayout() const noexcept { return layoutDepth != 0; }
        LayoutContext &currentLayoutContext() noexcept { return layoutStack[layoutDepth - 1]; }
        const LayoutContext &currentLayoutContext() const noexcept { return layoutStack[layoutDepth - 1]; }

        /// 压入一层布局区域。返回是否成功：栈满时返回 false，调用方**不能**再配对地弹一次
        /// （那样弹掉的是外层宿主压入的区域）。beginWindow() 把结果记在 WindowFrame 里。
        bool pushLayoutArea(Vector2 minPoint, Vector2 maxPoint) noexcept {
            if (layoutDepth >= MaxLayoutDepth) {
                REZERO_ASSERT_MSG(false, "layout region nesting exceeds MaxLayoutDepth");
                return false;
            }
            layoutStack[layoutDepth].reset(minPoint, maxPoint);
            layoutDepth += 1;
            return true;
        }
        void popLayoutArea() noexcept {
            if (layoutDepth > 0) {
                layoutDepth -= 1;
            }
        }

        /// Reserves \p size for an item and returns its screen space rectangle.
        Rect placeItem(Vector2 size) noexcept {
            if (!hasLayout()) {
                return Rect();
            }
            return currentLayoutContext().place(size, style().itemSpacing);
        }

        auto isItemVisible(const Rect &rect) const noexcept -> bool {
            if (!hasLayout()) {
                return false;
            }
            if (currentWindowIndex() != NoWindow && Windows[currentWindowIndex()].skipItems) {
                return false;
            }
            return ForegroundList.currentClipRect().overlaps(rect);
        }

        // 检查鼠标是否位于给定的矩形区域内，并且能够与该控件进行交互。
        auto isMouseHoveringRect(const Rect &rect, Id id = 0) const noexcept -> bool {
            const Vector2 mouse = Input.currentMousePosition();
            if (!rect.contains(mouse)) {
                return false;
            }
            // skipItems 只适用于窗口（处于折叠状态时）；面板和帧仍然会接收输入
            if (currentWindowIndex() != NoWindow) {
                const WindowDescription &window = Windows[currentWindowIndex()];
                if (window.skipItems) {
                    return false;
                }
                // 只有当一个窗口位于最上层时，它才能响应光标所在位置的输入
                // 否则，被其他窗口遮挡的窗口中的控件仍然会接收鼠标点击，而下面的窗口就永远无法从其他窗口下方被提升到前面。
                if (windowIsCovered(window.id, mouse)) {
                    return false;
                }
            }
            // 弹出窗口（Popup）对鼠标具有模态性，点击弹窗外部时，只应该关闭弹窗，不应该同时点击弹窗下方被遮挡的控件
            if (state.popup.id != InvalidId && !state.popup.bounds.contains(mouse)) {
                return false;
            }
            if (!ForegroundList.currentClipRect().contains(mouse)) {
                return false;
            }
            if (state.interaction.active != InvalidId && state.interaction.active != id) {
                return false;
            }
            return true;
        }

        /// 接管鼠标：从现在起，只有 \p id 能收到鼠标事件，直到 releaseMouse()。
        ///
        /// 每帧都要由属主重新确认一次（claimMouse() 或 holdMouse()）。少了这一步，
        /// 属主在用户松开鼠标那一帧恰好没被声明的话，active 就永远没人清了；见
        /// InteractionState 的说明。
        void claimMouse(Id id) noexcept {
            state.interaction.active = id;
            state.interaction.frame = FrameIndex;
        }

        /// 属主本帧还在（按住中/拖动中），刷新它的归属帧号，让 endFrame() 不要扫掉它。
        void holdMouse(Id id) noexcept {
            if (state.interaction.active == id) {
                state.interaction.frame = FrameIndex;
            }
        }

        /// 属主主动结束这次鼠标操作（通常是看到鼠标松开）。
        void releaseMouse() noexcept { state.interaction.active = InvalidId; }

        auto clickBehavior(const Rect &rect, Id id, bool enabled = true) noexcept -> InteractionResult {
            InteractionResult result;
            result.hovered = enabled && isMouseHoveringRect(rect, id);
            // 鼠标左键按下
            if (result.hovered && Input.isMouseClicked(Input::MouseButton::Left)) {
                claimMouse(id);
            }
            if (state.interaction.active == id) {
                // 鼠标左键持续按住
                if (Input.isMouseDown(Input::MouseButton::Left)) {
                    holdMouse(id);
                    result.held = true;
                } else {
                    // 鼠标左键已经松开
                    result.clicked = rect.contains(Input.currentMousePosition());
                    // 清除活动控件
                    releaseMouse();
                }
            }
            return result;
        }

        /// Builds the glyph atlas once and uploads it as a texture.
        ///
        /// The atlas is rasterized at the style's font size, so the value of
        /// Style::fontSize on the first frame decides the glyph resolution;
        /// changing it later only moves layout around.
        void ensureFontAtlas() noexcept {
            if (fontTexture != InvalidTexture) {
                return;
            }

            const ReZeroTrueType::Image *image = Atlas.atlasImage(Atlas.defaultFont());
            if (image == nullptr) {
                return;
            }
            fontTexture = textureList.createTexture(image->width, image->height,
                                                    reinterpret_cast<const std::uint8_t *>(image->data()));
        }

        auto calculateTextSize(u8StringView text, FontId font = InvalidFont, float maxLineWidth = 0.0f) const noexcept
            -> Vector2 {
            const FontId effectiveFont = resolveFont(font);
            if (effectiveFont == InvalidFont) {
                return Vector2();
            }

            const float lineHeight = Atlas.lineHeight(effectiveFont);
            float width = 0.0f;
            float maximumWidth = 0.0f;
            float lineCount = 1.0f;

            std::size_t offset = 0;
            while (offset < text.size()) {
                auto codepoint = unicode::decodeUtf8(text, offset);
                if (!codepoint) {
                    break;
                }
                if (*codepoint == '\n') {
                    maximumWidth = maximum(maximumWidth, width);
                    width = 0.0f;
                    lineCount += 1.0f;
                    continue;
                }
                const float advance = Atlas.glyphAdvance(effectiveFont, *codepoint);
                if (maxLineWidth > 0.0f && width > 0.0f && width + advance > maxLineWidth) {
                    maximumWidth = maximum(maximumWidth, width);
                    width = 0.0f;
                    lineCount += 1.0f;
                }
                // 测量文字宽度时，必须采用和实际绘制文字时完全相同的光标移动规则，否则测量结果就会和最终绘制的位置对不上
                width += advance;
            }
            return Vector2(maximum(maximumWidth, width), lineCount * lineHeight);
        }

        // 把 text 绘制到 position 指定的位置，并以它作为文本区域的左上角
        // 支持显式换行和基于宽度的自动换行；wrapWidth 控制自动换行的阈值。
        void renderTextToDrawList(DrawList &list, Vector2 position, u8StringView text, Pixel color, FontId font,
                                  float wrapWidth) noexcept {
            const FontId effectiveFont = resolveFont(font);
            if (effectiveFont == InvalidFont || fontTexture == InvalidTexture) {
                return;
            }

            const float lineHeight = Atlas.lineHeight(effectiveFont);
            const float startX = position.x;
            // 文字绘制时的当前位置，可以理解为绘制光标。
            float penX = position.x;
            // 文字绘制的基准位置（pen）位于基线上，而基线距离行框顶部的距离就是一个 ascent 的高度。
            float baselineY = position.y + Atlas.ascent(effectiveFont);

            std::size_t offset = 0;
            while (offset < text.size()) {
                auto codepoint = unicode::decodeUtf8(text, offset);
                if (!codepoint) {
                    break;
                }
                if (*codepoint == '\n') {
                    penX = startX;
                    baselineY += lineHeight;
                    continue;
                }

                float nextX = penX;
                GlyphQuad quad{};
                if (!Atlas.getGlyphQuad(effectiveFont, *codepoint, &nextX, &baselineY, quad)) {
                    continue;
                }

                // Only wrap once something has been drawn on this line, otherwise
                // a single over-long glyph would wrap forever.
                if (wrapWidth > 0.0f && penX > startX && nextX - startX > wrapWidth) {
                    penX = startX;
                    baselineY += lineHeight;
                    nextX = penX;
                    if (!Atlas.getGlyphQuad(effectiveFont, *codepoint, &nextX, &baselineY, quad)) {
                        continue;
                    }
                }

                // Blank glyphs report success with empty UVs so that the pen still
                // advances; they must not produce geometry.
                if (quad.uv0.x != quad.uv1.x && quad.uv0.y != quad.uv1.y) {
                    list.addTexturedQuad(quad.position0, quad.position1, quad.uv0, quad.uv1, color, fontTexture);
                }
                // Tracking rides on top of the glyph's own advance, matching what
                // measureText adds so the two never drift apart.
                penX = nextX;
            }
        }
        void renderText(Vector2 position, u8StringView text, Pixel color, FontId font, float wrapWidth) noexcept {
            renderTextToDrawList(ForegroundList, position, text, color, font, wrapWidth);
        }
        /// Resolves the "InvalidFont means the default" convention and folds in
        /// the "atlas is not usable yet" case.
        auto resolveFont(FontId font) const noexcept -> FontId {
            if (!Atlas.isBuilt()) {
                return InvalidFont;
            }
            const FontId effective = font == InvalidFont ? Atlas.defaultFont() : font;
            return effective;
        }

        auto fonts() -> FontAtlas & { return Atlas; }
        auto font() -> FontId { return fonts().defaultFont(); }

        // 一帧的用法（beginFrame/endFrame 这一对在下面，和窗口几何那些内部函数同一段代码里）：
        //
        //     ctx.beginFrame(deltaTime);
        //     { WindowScope window(ctx, u8"Panel"); window.ui(...); }
        //     ctx.endFrame();
        //     renderer.renderDrawData(ctx.drawItems());
        //
        // 不再有 RAII 守卫：帧的起止由宿主显式调用。代价是忘记 endFrame() 时不会崩，
        // 只是静默少做了收尾（窗口置顶失效、本帧输入泄漏到下一帧），所以两边都加了断言。

      private:
        // 统一计算窗口在屏幕上的布局尺寸和位置
        struct WindowGeometry {
            float titleBarHeight = 0.0f;
            Rect titleBar;
            Rect collapseTriangle;
            // 折叠箭头自己的唯一标识符
            Id collapseId = InvalidId;
            // 表示折叠箭头是否启用交互。
            bool hasCollapseHandle = false;
            // 窗口实际绘制内容和裁剪区域所对应的矩形
            Rect body;
            // 窗口内部供控件布局使用的区域,窗口折叠后，会把内容区域设置为空区域。
            Vector2 contentMin; // 内容区域的最小坐标，通常对应左上角。
            Vector2 contentMax; // 内容区域的最大坐标，通常对应右下角。
            // 是否完全跳过窗口内部控件的处理, 当 skipItems 为 true
            // 时，窗口仍然可以显示标题栏，但内部控件不再正常参与处理。
            bool skipItems = false;
        };

        auto windowGeometry(const WindowDescription &window) const noexcept -> WindowGeometry {
            WindowGeometry geometry;
            geometry.titleBarHeight = window.hasTitleBar ? Style.fontSize + Style.framePadding.y * 2.0f : 0.0f;
            geometry.titleBar = Rect(window.rect.x, window.rect.y, window.rect.width, geometry.titleBarHeight);

            // 箭头保持其作为独立三角形时的原有尺寸：
            // 它只是绘制在正方形区域内部的一个标记，而鼠标命中检测针对的是整个正方形区域。
            const float triangleSize = Style.fontSize * 0.6f;
            geometry.collapseId = hashCombine(window.id, Id(0x85EBCA77u));
            geometry.collapseTriangle = Rect(geometry.titleBar.x + Style.framePadding.x,
                                             geometry.titleBar.y + geometry.titleBarHeight * 0.5f - triangleSize * 0.5f,
                                             triangleSize, triangleSize);
            geometry.hasCollapseHandle = window.hasTitleBar && window.drawBackground;

            const float bodyHeight = window.collapsed ? geometry.titleBarHeight : window.rect.height;
            geometry.body = Rect(window.rect.x, window.rect.y, window.rect.width, bodyHeight);

            geometry.contentMin =
                Vector2(window.rect.x + window.padding.x, window.rect.y + geometry.titleBarHeight + window.padding.y);
            geometry.contentMax =
                Vector2(maximum(geometry.contentMin.x, window.rect.x + window.rect.width - window.padding.x),
                        maximum(geometry.contentMin.y, window.rect.y + window.rect.height - window.padding.y));

            geometry.skipItems = window.collapsed;
            if (window.collapsed) {
                geometry.contentMax = geometry.contentMin;
            }
            return geometry;
        }

      public:
        /// 一帧的开始：清掉上一帧已被渲染器消费掉的数据，并把本帧的登记表、布局深度和 arena
        /// 归零；同时用上一帧的布局判断光标下是哪个窗口（顺带处理"点击置顶"）。
        ///
        /// 必须和 endFrame() 成对调用：
        ///
        ///     ctx.beginFrame(deltaTime);
        ///     { WindowScope window(ctx, u8"Panel"); window.ui(...); }
        ///     ctx.endFrame();          // 窗口作用域必须先析构
        ///     renderer.renderDrawData(ctx.drawItems());
        ///
        /// 忘记 endFrame() 的后果不是崩溃而是静默错乱：窗口照样会画出来，但
        /// reorderWindowGeometry() 没跑（点窗口置顶失效）、Input::onFrameEnd() 也没跑
        /// （本帧的点击/按键/输入字符泄漏到下一帧，于是点一次触发两次）。
        /// Debug 下两边都有断言看着，见下面 frameOpen。
        void beginFrame(float deltaTime = 0.0f) noexcept {
            REZERO_ASSERT_MSG(!frameOpen, "beginFrame() without a matching endFrame()");
            frameOpen = true;

            FrameIndex += 1;
            // One long frame (a breakpoint, a resize storm) must not teleport an animation, so
            // the step is capped; 0.1f keeps the literal a float, not a double.
            FrameDelta = clampFloat(deltaTime, 0.0f, 0.1f);

            BackgroundList.onFrameBegin();
            ForegroundList.onFrameBegin();
            OverlayList.onFrameBegin();
            BackgroundList.setDisplayRect(Rect(Vector2(), DisplaySize));
            ForegroundList.setDisplayRect(Rect(Vector2(), DisplaySize));
            OverlayList.setDisplayRect(Rect(Vector2(), DisplaySize));

            Input.onFrameBegin(deltaTime);
            // Rasterizes the glyph atlas and publishes it as a texture the first
            // time a frame starts; afterwards this is two flag checks.
            ensureFontAtlas();

            layoutDepth = 0;

            // Resolve which window sits under the cursor using last frame's layout;
            // windows declared later in a frame are considered on top.
            hoveredWindowId = InvalidId;
            // Bottom first, so the last window containing the cursor wins: that is the one
            // the user sees on top.
            //
            // rect 是"最后一次画它时"留下的，所以还要看那一帧是不是刚过去：一个已经不画的窗口
            // 不该继续占着光标（否则它的悬停高亮和点击置顶都会作用在用户看不见的地方）。
            for (const Id id : WindowOrder) {
                const WindowDescription *window = findWindow(id);
                if (window != nullptr && window->drawnFrame + 1 >= FrameIndex &&
                    window->rect.contains(Input.currentMousePosition())) {
                    hoveredWindowId = id;
                }
            }

            // Clicking a window raises it. The stacking order is what the frame is drawn in,
            // so this is what makes the clicked window cover the others from this frame on.
            if (hoveredWindowId != InvalidId && Input.isMouseClicked(Input::MouseButton::Left)) {
                bringWindowToFront(hoveredWindowId);
            }

            // 登记表按帧重建，上一帧留下的条目在这里作废。深度也归零：配对正常时它本来就是 0，
            // 归零是为了让宿主跨帧遗留的 WindowScope 不至于让下一帧读到越界的条目（那种情况下
            // endWindow() 会自己挡掉并断言，而不是越界访问）。
            WindowFrames.clear();
            WindowDepth = 0;

            // 每帧 scratch 同理：上一帧格式化出来的文本到这里作废。控件每帧都会重新声明，
            // 所以那些视图到帧末就已经没用了；块链留着复用，稳态下这里不会分配。
            Arena.reset();

            keyboardCapture = false;
        }

        /// 一帧的结束：清掉本帧没出现过的属主留下的过期状态（焦点、弹窗、按键监听、鼠标归属、
        /// 动画槽），按堆叠顺序重排窗口几何，并产出渲染器要读的 DrawData。
        ///
        /// 之后才能读 drawItems()；反过来，窗口作用域必须在调用它之前析构
        /// （WindowScope 的析构会 endWindow()，那要用本帧的登记表）。
        void endFrame() noexcept {
            REZERO_ASSERT_MSG(frameOpen, "endFrame() without a matching beginFrame()");
            frameOpen = false;

            state.endFrame(FrameIndex);

            reorderWindowGeometry();

            drawData.background = &BackgroundList;
            drawData.foreground = &ForegroundList;
            drawData.overlay = &OverlayList;
            drawData.resourceCommands = textureList.span();
            drawData.displaySize = DisplaySize;
            drawData.valid = true;

            Input.onFrameEnd();
        }

      private:

        /// The entry in `WindowFrames` that belongs to \p id, or null when that window was not
        /// drawn this frame. The two lists are not the same length: `WindowOrder` keeps every
        /// window that was ever declared, while `WindowFrames` only holds the ones this frame
        /// declared.
        auto findRun(Id id) const noexcept -> const WindowFrame * {
            for (const WindowFrame &candidate : WindowFrames) {
                if (candidate.id == id) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        /// True when the stacking order already equals the order the windows were declared
        /// in, i.e. when the replay below would rebuild the command stream onto itself.
        ///
        /// `WindowFrames` is in declaration order, so the two orders agree exactly when the
        /// k-th window of `WindowOrder` that was declared this frame is `WindowFrames[k]`. That
        /// holds on every frame except the ones following a click on a window that was not
        /// already on top, which is what `beginFrame` raises.
        auto stackingMatchesDeclarationOrder() const noexcept -> bool {
            std::size_t slot = 0;
            for (const Id id : WindowOrder) {
                const WindowFrame *run = findRun(id);
                if (run == nullptr) {
                    continue; // declared in an earlier frame and not drawn in this one
                }
                if (slot >= WindowFrames.size() || run != &WindowFrames[slot]) {
                    return false;
                }
                slot += 1;
            }
            return true;
        }

        /// Windows draw where they are declared, but the order the user sees is the stacking
        /// order. Rebuilding the per-window command ranges in that order - leaving whatever was
        /// drawn between them where it is - is what makes a raised window cover the others.
        ///
        /// Only the command array is rebuilt. Vertices and indices are left exactly as they are,
        /// because a command carries the index range it draws and its own clip rectangle, and the
        /// renderer walks commands and nothing else: the command order *is* the draw order.
        /// Copying the vertices too would cost hundreds of times more and change nothing.
        void reorderWindowGeometry() noexcept {
            if (WindowFrames.size() < 2) {
                return;
            }

            // The replay rebuilds the whole command array, so it is skipped while the two orders
            // already agree.
            if (stackingMatchesDeclarationOrder()) {
                return;
            }

            const Span<const DrawCommand> commands = ForegroundList.currentCommands();
            const std::uint32_t commandTotal = ForegroundList.commandCount();

            reorderScratch.clear();
            const auto appendRange = [&](std::uint32_t begin, std::uint32_t end) {
                for (std::uint32_t index = begin; index < end; ++index) {
                    reorderScratch.push_back(commands[index]);
                }
            };

            // Declaration order supplies the gaps and stacking order supplies the content:
            // output slot k holds the window that is k-th from the bottom, and everything
            // drawn between two windows keeps its place in the stream.
            std::uint32_t commandCursor = 0;
            std::size_t slot = 0;
            for (const Id id : WindowOrder) {
                const WindowFrame *source = findRun(id);
                if (source == nullptr) {
                    continue; // declared in an earlier frame and not drawn in this one
                }
                // Every drawn window is in the stacking order, so the slots run out exactly
                // when the walk does.
                REZERO_ASSERT_MSG(slot < WindowFrames.size(), "a drawn window has no declaration slot");
                const WindowFrame &extent = WindowFrames[slot];

                // Whatever was drawn since the previous run keeps its place in the stream.
                appendRange(commandCursor, extent.commandBegin);
                appendRange(source->commandBegin, source->commandEnd);
                commandCursor = extent.commandEnd;
                slot += 1;
            }
            appendRange(commandCursor, commandTotal);

            // Vertices and indices stay where they are, so only the commands are replaced.
            ForegroundList.clearCommands();
            ForegroundList.appendCommands(reorderScratch.span());
        }

      public:
        // template <RendererLike R> void draw(R &renderer) { renderer.render(...); }
        auto drawItems() noexcept -> DrawData & { return drawData; }

        /// 每帧 scratch。宿主可以拿它放自己的临时数据
        /// 拿到的内存在本帧内一直有效，下一帧开始时作废。
        auto arena() noexcept -> FrameArena & { return Arena; }

        /// Ends the current line so the next item starts below this one.
        ///
        /// 每个控件默认就独占一行，所以只有用过 sameLine() 之后才需要它；
        /// 在一行还没放东西时调用它，效果是插入一个 itemSpacing.y 的空隙。
        void newLine() noexcept {
            if (hasLayout()) {
                currentLayoutContext().wrap(style().itemSpacing);
            }
        }

        /// 让下一个控件接着当前这一行放，而不是另起一行；见 LayoutContext::sameLine()。
        ///
        /// 配合 contentRegionWidth() 就能做到"标签 + 占满剩余宽度的控件"这类同行排布：
        ///     ui.text(u8"Exposure");
        ///     ui.sameLine();
        ///     ui.slider<float>(u8"").value(&exposure).range(0.0f, 2.0f);
        void sameLine() noexcept {
            if (hasLayout()) {
                currentLayoutContext().sameLine();
            }
        }

        /// Width left for the next item on the current line. Zero when no container
        /// is open, which makes a widget fall back to its own default.
        auto contentRegionWidth() const noexcept -> float {
            return hasLayout() ? currentLayoutContext().availableWidth() : 0.0f;
        }
        DrawList &drawList() noexcept { return ForegroundList; }

        Style &style() noexcept { return Style; }

        /// 由标签和当前作用域（窗口）算出的控件标识。
        ///
        /// 不是简单地"用作用域 id 当 FNV 的种子"：那样一个空标签会得到 seed 本身，也就是窗口
        /// 自己的 id（`hashString(u8"")==seed`），于是没有标签的控件全都变成"窗口本身"，
        /// 彼此也不再区分；而且单字节标签的种子组合在异或意义上是线性的。这里改成先各自哈希、
        /// 再混一次，空标签只会得到一个普通的、与窗口 id 无关的值。
        auto id(u8StringView seed) const noexcept -> Id { return hashCombine(idSeed(), hashString(seed)); }

        /// Seconds since the previous frame, clamped so one long frame cannot
        /// teleport an animation. Zero while the host does not pass it to
        /// beginFrame().
        auto deltaTime() const noexcept -> float { return FrameDelta; }

        auto frame() { return FrameIndex; }

        /// The layer drawn after every window: where a popup belongs, because a popup has
        /// to cover the widgets the frame declared after the one that opened it.
        auto overlay() noexcept -> DrawList & { return OverlayList; }

        auto persistentState() -> PersistentState & { return state; }
        /// Read-only view, for the widget code that only inspects state (a caret offset, the
        /// focused field) and takes a const Context to say so.
        auto persistentState() const -> const PersistentState & { return state; }

      private:
        auto idSeed() const noexcept -> Id { return WindowDepth == 0 ? 2166136261u : WindowFrames.back().id; }
        auto currentWindowIndex() const noexcept -> std::size_t {
            return WindowDepth == 0 ? NoWindow : WindowFrames.back().windowIndex;
        }

      private:
        Input Input;

        Vector2 DisplaySize;

        Style Style;
        ArrayList<WindowDescription> Windows;

        // 窗口的层叠顺序，按从底层到顶层排列：最后一个元素就是最上层的窗口。
        // 用户点击哪个窗口，就将它移动到列表末尾。
        // 每一帧都按照这个顺序绘制窗口，而不是按照窗口在代码中声明的先后顺序绘制。
        ArrayList<Id> WindowOrder;
        // 本帧被声明过的窗口，按声明顺序，并保留到帧末：帧结束时要按堆叠顺序重排这些命令区间。
        // 只追加、不弹出，所以最后一条就是此刻打开着的那个窗口。
        ArrayList<WindowFrame> WindowFrames;
        /// 此刻打开了几个窗口。窗口不允许嵌套，所以它只会是 0 或 1。它只回答"现在是否在某个
        /// 窗口体内"（idSeed / currentWindowIndex）；当前是哪一条由 WindowFrames.back() 给出。
        std::size_t WindowDepth = 0;

        Vector2 WindowDragOffset;
        Id hoveredWindowId = InvalidId;

        DrawList BackgroundList;
        DrawList ForegroundList;
        DrawList OverlayList;
        /// Scratch the window commands are reordered into; see reorderWindowGeometry(). It holds
        /// commands only - the vertices and indices never move, so it needs nothing else.
        ArrayList<DrawCommand> reorderScratch;
        textureCommandList textureList;

        FontAtlas Atlas;
        TextureId fontTexture = InvalidTexture;

        /// beginFrame() 每帧把它归零，块链复用，所以稳态下每帧零分配。
        FrameArena Arena;

        /// Fixed size, so a frame never allocates and the LayoutContext references handed
        /// out by currentLayoutContext() stay valid across pushLayoutArea().
        Array<LayoutContext, MaxLayoutDepth> layoutStack{};
        std::size_t layoutDepth = 0;

        bool keyboardCapture = false;
        DrawData drawData;

        std::uint32_t FrameIndex = 0;
        /// 帧是否已被 beginFrame() 打开而还没被 endFrame() 关掉；只用于断言配对。
        bool frameOpen = false;

        float FrameDelta = 0.0f;
        PersistentState state;
    };
} // namespace ReZeroGui
