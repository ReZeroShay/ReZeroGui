#pragma once
#include <concepts>
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Context.h>
#include <ReZeroGui/Format.h>
#include <ReZeroGui/State.h>
#include <ReZeroGui/Style.h>

namespace ReZeroGui {
    class ButtonBuilder;
    class TextBuilder;
    struct CheckboxBuilder;
    class RadioButtonBuilder;
    class SwitchToggleBuilder;
    class KeyBindBuilder;
    template <typename T> class SliderBuilder;
    struct TextInputBuilder;
    class ProgressBarBuilder;
    class SeparatorBuilder;
    class SeparatorTextBuilder;

    namespace detail {

        inline void renderFrame(Context &ctx, const Rect &rect, Pixel fill, Pixel border, float rounding,
                                bool hasBorder = true) noexcept {
            DrawList &list = ctx.drawList();
            list.addRectFilled(rect.position(), rect.maxPoint(), fill, rounding);
            if (hasBorder && ctx.style().frameBorderSize > 0.0f) {
                list.addRect(rect.position(), rect.maxPoint(), border, rounding, DrawCorner::All,
                             ctx.style().frameBorderSize);
            }
        }

        inline void renderCheckMark(Context &ctx, const Rect &rect, Pixel color) noexcept {
            const float thickness = maximum(1.0f, rect.width * 0.16f);
            const Vector2 start(rect.x + rect.width * 0.20f, rect.y + rect.height * 0.52f);
            const Vector2 middle(rect.x + rect.width * 0.42f, rect.y + rect.height * 0.76f);
            const Vector2 end(rect.x + rect.width * 0.80f, rect.y + rect.height * 0.24f);
            DrawList &list = ctx.drawList();
            list.addLine(start, middle, color, thickness);
            list.addLine(middle, end, color, thickness);
        }

        /// Byte offset in `buffer` whose rendered prefix is closest to the click at `mouseX`.
        inline auto cursorOffsetFromX(Context &ctx, Span<char8_t> buffer, float textX, float mouseX) noexcept
            -> std::size_t {
            const float target = mouseX > textX ? mouseX - textX : 0.0f;
            const std::size_t length = textLength(buffer);
            const u8StringView bytes(reinterpret_cast<const char8_t *>(buffer.data()), length);

            std::size_t best = 0;
            float bestDistance = target;
            std::size_t offset = 0;
            while (offset < length) {
                const std::size_t next = unicode::nextCodepoint(bytes, offset);
                const float width = ctx.calculateTextSize(u8StringView(buffer.data(), next)).x;
                const float distance = width > target ? width - target : target - width;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = next;
                }
                offset = next;
            }
            return best;
        }

        /// Caret of the text field that owns the keyboard, as a byte offset.
        inline auto caretPosition(const Context &ctx) noexcept -> std::size_t {
            return ctx.persistentState().textEdit.caret;
        }

        inline auto isTextFocused(const Context &ctx, Id id) noexcept -> bool {
            return id != InvalidId && ctx.persistentState().textEdit.focused == id;
        }

        /// Advances the animation value owned by \p id; see AnimationState::advance.
        inline auto animate(Context &ctx, Id id, float target, float tau) noexcept -> float {
            return ctx.persistentState().animations.advance(id, target, tau, ctx.deltaTime(), ctx.frame());
        }

        /// Shared drag behavior for a value track. Only the slider uses it today, but it is the
        /// same hover / mouse-owner bookkeeping clickBehavior does, one axis up.
        inline auto dragBehavior(Context &ctx, const Rect &rect, Id id, float value, float minimum, float maximum,
                                 float grabWidth, bool enabled = true) noexcept -> DragResult {
            DragResult result;
            result.value = value;
            result.hovered = enabled && ctx.isMouseHoveringRect(rect, id);

            PersistentState &state = ctx.persistentState();
            if (result.hovered && ctx.io().isMouseClicked(Input::MouseButton::Left)) {
                ctx.claimMouse(id);
            }
            if (state.interaction.active != id) {
                return result;
            }
            if (!ctx.io().isMouseDown(Input::MouseButton::Left)) {
                ctx.releaseMouse();
                return result;
            }
            // 还在拖：每帧重新确认归属，这样控件消失时 endFrame() 能把它扫掉。
            ctx.holdMouse(id);

            const float trackWidth = ReZeroGui::maximum(1.0f, rect.width - grabWidth);
            const float normalized =
                clampFloat((ctx.io().currentMousePosition().x - rect.x - grabWidth * 0.5f) / trackWidth, 0.0f, 1.0f);
            result.value = minimum + (maximum - minimum) * normalized;
            result.changed = true;
            return result;
        }

        /// Handles a key bind: clicking starts listening, the next key press becomes the binding.
        inline auto keyBindBehavior(Context &ctx, const Rect &rect, Id id, Input::KeyCode key,
                                    bool enabled = true) noexcept -> KeyBindResult {
            KeyBindResult result;
            result.key = key;

            const InteractionResult click = ctx.clickBehavior(rect, id, enabled);
            result.interaction = click;

            PersistentState &state = ctx.persistentState();
            if (!enabled) {
                if (state.keyBind.listening == id) {
                    state.keyBind.listening = InvalidId;
                }
                return result;
            }

            if (click.clicked) {
                state.keyBind.listening = state.keyBind.listening == id ? InvalidId : id;
            }
            if (state.keyBind.listening != id) {
                return result;
            }

            result.listening = true;
            state.keyBind.frame = ctx.frame();
            ctx.captureKeyboard(true);

            // Index 0 is KeyCode::None, which is not something the user can press.
            for (std::size_t index = 1; index < Input::KeyCodeCount; ++index) {
                const auto candidate = static_cast<Input::KeyCode>(index);
                if (!ctx.io().isKeyPressed(candidate)) {
                    continue;
                }
                if (candidate == Input::KeyCode::Escape) {
                    state.keyBind.listening = InvalidId;
                    result.listening = false;
                    return result;
                }
                // Backspace and Delete clear the binding instead of becoming it.
                result.key = candidate == Input::KeyCode::Backspace || candidate == Input::KeyCode::Delete
                                 ? Input::KeyCode::None
                                 : candidate;
                result.changed = true;
                result.listening = false;
                state.keyBind.listening = InvalidId;
                return result;
            }
            return result;
        }

        /// Keyboard editing for a single line text field.
        inline auto textInputBehavior(Context &ctx, const Rect &rect, Id id, Span<char8_t> buffer) noexcept -> bool {
            bool changed = false;
            if (buffer.size() < 2) {
                return false;
            }

            PersistentState &state = ctx.persistentState();
            const bool hovered = ctx.isMouseHoveringRect(rect, id);

            const bool clicked = ctx.io().isMouseClicked(Input::MouseButton::Left);
            if (hovered && clicked) {
                state.textEdit.focused = id;
                state.textEdit.caret = cursorOffsetFromX(ctx, buffer, rect.x + ctx.style().framePadding.x,
                                                         ctx.io().currentMousePosition().x);
            } else if (clicked && state.textEdit.focused == id) {
                state.textEdit.focused = InvalidId;
            }

            if (state.textEdit.focused != id) {
                return changed;
            }

            state.textEdit.frame = ctx.frame();
            const std::size_t length = textLength(buffer);
            if (state.textEdit.caret > length) {
                state.textEdit.caret = length;
            }

            ctx.captureKeyboard(true);
            const u8StringView bytes(reinterpret_cast<const char8_t *>(buffer.data()), length);

            // Control characters arrive as code points too; they are not editable text.
            for (const char32_t codepoint : ctx.io().currentCharacters()) {
                if (codepoint < 0x20 || codepoint == 0x7F) {
                    continue;
                }
                char encoded[4];
                const std::size_t count = unicode::encodeUtf8(static_cast<std::uint32_t>(codepoint), encoded);
                if (insertText(buffer, state.textEdit.caret, encoded, count)) {
                    state.textEdit.caret += count;
                    changed = true;
                }
            }

            if (ctx.io().isKeyPressed(Input::KeyCode::Backspace) && state.textEdit.caret > 0) {
                const std::size_t previous = unicode::previousCodepoint(bytes, state.textEdit.caret);
                if (eraseText(buffer, previous, state.textEdit.caret)) {
                    state.textEdit.caret = previous;
                    changed = true;
                }
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::Delete) && state.textEdit.caret < length) {
                const std::size_t next = unicode::nextCodepoint(bytes, state.textEdit.caret);
                if (eraseText(buffer, state.textEdit.caret, next)) {
                    changed = true;
                }
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::LeftArrow)) {
                state.textEdit.caret = unicode::previousCodepoint(bytes, state.textEdit.caret);
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::RightArrow)) {
                state.textEdit.caret = unicode::nextCodepoint(bytes, state.textEdit.caret);
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::Home)) {
                state.textEdit.caret = 0;
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::End)) {
                state.textEdit.caret = length;
            }
            if (ctx.io().isKeyPressed(Input::KeyCode::Enter) || ctx.io().isKeyPressed(Input::KeyCode::Escape)) {
                state.textEdit.focused = InvalidId;
            }
            return changed;
        }

    } // namespace detail

    class Ui {
      public:
        explicit Ui(Context &ctx) noexcept : Ctx(ctx) {}

        Ui(const Ui &) noexcept = delete;
        Ui &operator=(const Ui &) noexcept = delete;

        auto ctx() noexcept -> Context & { return Ctx; }

        // ---- widget factories (defined after the builders, which they return) ----
        //
        // 标签里可以写一次 "##"：前一半显示给用户，后一半当控件身份。于是改显示文本不会丢状态
        // （u8"音量##volume" 和 u8"Volume##volume" 是同一个滑块），而同一个窗口里两个同名控件
        // 可以用后缀分开（u8"Reset##left" / u8"Reset##right"，只写 "Reset" 会撞 ID）。
        // 没有 "##" 时整串既是显示文本也是身份。详见 Id.h；窗口标题同样走这个约定。
        // text() 和 separatorText() 画的是内容而不是标签，所以那里的 "##" 是字面文本。
        /// text()，但先把 \p format 格式化：每个 "{}" 依次换成下一个参数，"{{" / "}}" 换成
        /// 单个花括号。没有 format spec。返回值链式 API 与 text() 完全一致。
        ///
        /// 格式化后的字节写进 Context 的每帧 scratch（一块可复用的 bump 内存），寿命到本帧结束：
        /// 和 text() 的字面量一样，足够活过 build()。缓冲区大小按参数上界一次要够，所以不需要
        /// "写满再扩容"那套机制。类型不支持是编译期错误。
        template <class... Args> auto text(u8StringView format, const Args &...args) noexcept -> TextBuilder;
        auto button(u8StringView text) noexcept -> ButtonBuilder;
        auto checkbox(u8StringView text) noexcept -> CheckboxBuilder;
        auto radioButton(u8StringView text, int optionValue) noexcept -> RadioButtonBuilder;
        template <typename T> auto slider(u8StringView text) noexcept -> SliderBuilder<T>;
        auto inputText(u8StringView text) noexcept -> TextInputBuilder;
        auto progressBar() noexcept -> ProgressBarBuilder;
        auto switchToggle(u8StringView text) noexcept -> SwitchToggleBuilder;
        auto separator() noexcept -> SeparatorBuilder;
        auto separatorText(u8StringView text) noexcept -> SeparatorTextBuilder;
        auto keybind(u8StringView text) noexcept -> KeyBindBuilder;

        /// 让下一个控件接着当前这一行放，而不是另起一行；见 LayoutContext::sameLine()。
        void sameLine() noexcept { Ctx.sameLine(); }

        // ---- widget interaction behavior, forwarded to detail ----

        auto dragBehavior(const Rect &rect, Id id, float value, float minimum, float maximum, float grabWidth,
                          bool enabled = true) noexcept -> DragResult {
            return detail::dragBehavior(Ctx, rect, id, value, minimum, maximum, grabWidth, enabled);
        }

        auto keyBindBehavior(const Rect &rect, Id id, Input::KeyCode key, bool enabled = true) noexcept
            -> KeyBindResult {
            return detail::keyBindBehavior(Ctx, rect, id, key, enabled);
        }

        auto cursorOffsetFromX(Span<char8_t> buffer, float textX, float mouseX) noexcept -> std::size_t {
            return detail::cursorOffsetFromX(Ctx, buffer, textX, mouseX);
        }

        auto textInputBehavior(const Rect &rect, Id id, Span<char8_t> buffer) noexcept -> bool {
            return detail::textInputBehavior(Ctx, rect, id, buffer);
        }

        auto caretPosition() const noexcept -> std::size_t { return detail::caretPosition(Ctx); }

        auto isTextFocused(Id id) const noexcept -> bool { return detail::isTextFocused(Ctx, id); }

        auto animate(Id id, float target, float tau) noexcept -> float { return detail::animate(Ctx, id, target, tau); }
        auto arena() -> FrameArena & { return Ctx.arena(); }

      private:
        Context &Ctx;
    };

    class ButtonBuilder {
      public:
        ButtonBuilder(Ui &ui, u8StringView text) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)) {}

        ~ButtonBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }
        ButtonBuilder(const ButtonBuilder &) = delete;
        ButtonBuilder &operator=(const ButtonBuilder &) = delete;

        auto &&width(this auto &&self, float value) noexcept {
            self.ExplicitWidth = value;
            return std::forward<decltype(self)>(self);
        }

        auto &&fillWidth(this auto &&self, bool enabled = true) noexcept {
            self.IsFillWidth = enabled;
            return std::forward<decltype(self)>(self);
        }

        auto &&disabled(this auto &&self, bool value = true) noexcept {
            self.IsDisabled = value;
            return std::forward<decltype(self)>(self);
        }

        template <class F>
            requires std::invocable<F &, Ui &>
        auto &&onClick(this auto &&self, F &&callback) noexcept {
            self.clickCallback = Function<void(Ui &)>(callback);
            return std::forward<decltype(self)>(self);
        }

        auto clicked() noexcept -> bool {
            if (!IsBuilt) {
                build();
            }
            return IsClicked;
        }

        auto hovered() noexcept -> bool {
            if (!IsBuilt) {
                build();
            }
            return IsHovered;
        }

        auto pressed() noexcept -> bool {
            if (!IsBuilt) {
                build();
            }
            return IsHeld;
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const Vector2 textSize = ui.ctx().calculateTextSize(Text);
            Vector2 size = textSize + style.framePadding * 2.0f;
            if (IsFillWidth) {
                const float available = ui.ctx().contentRegionWidth();
                if (available > 0.0f) {
                    size.x = available;
                }
            }
            if (ExplicitWidth > 0.0f) {
                size.x = ExplicitWidth;
            }

            Rect = ui.ctx().placeItem(size);
            const InteractionResult interaction = ui.ctx().clickBehavior(Rect, Id, !IsDisabled);

            if (ui.ctx().isItemVisible(Rect)) {
                Pixel fill = style.pixel(PixelIndex::Button);
                if (IsDisabled) {
                    fill = fill * 0.55f;
                } else if (interaction.held) {
                    fill = style.pixel(PixelIndex::ButtonActive);
                } else if (interaction.hovered) {
                    fill = style.pixel(PixelIndex::ButtonHovered);
                }
                detail::renderFrame(ui.ctx(), Rect, fill, style.pixel(PixelIndex::Border), style.frameRounding,
                                    style.frameBorderSize > 0.0f);

                if (!Text.empty()) {
                    const Pixel textPixel =
                        IsDisabled ? style.pixel(PixelIndex::TextDisabled) : style.pixel(PixelIndex::Text);
                    const Vector2 textPosition = Rect.center() - textSize * 0.5f;
                    ui.ctx().renderText(textPosition, Text, textPixel, ui.ctx().font(), 0.0f);
                }
            }

            IsHovered = interaction.hovered;
            IsHeld = interaction.held;
            IsClicked = interaction.clicked && !IsDisabled;

            if (IsClicked && clickCallback) {
                clickCallback(ui);
            }
        }

      private:
        Ui &ui;
        Id Id;
        Rect Rect;
        u8StringView Text;
        float ExplicitWidth = 0.f;
        bool IsFillWidth = false;
        bool IsDisabled = false;
        bool IsBuilt = false;
        bool IsHovered = false;
        bool IsHeld = false;
        bool IsClicked = false;
        Function<void(Ui &)> clickCallback;
    };

    // 文本控件。
    //
    // 文本是借用的 u8StringView，builder 自己不持有任何字节：宿主传进来的字面量本来就在静态
    // 存储区；格式化出来的文本活在 Context 的每帧 arena 里（见 detail::format），同样活到本帧结束。
    // 所以这里没有 "指向自身的指针" 这类问题，builder 被移动也安全。
    //
    // 它没有 Id：文本不是可交互控件，没有状态要跨帧关联（对比其它 builder 都要 id(labelId())）。
    // 直接后果是 text() 里出现的 "##" 就是字面文本——内容控件不参与 "##" 约定，见 Id.h。
    // 顺带省掉每帧每个 text() 一次字符串哈希。
    class TextBuilder {
      public:
        TextBuilder(Ui &ui, u8StringView text) noexcept
            : ui(ui), Text(text), Pixel(ui.ctx().style().pixel(PixelIndex::Text)), Font(ui.ctx().font()) {}

        ~TextBuilder() noexcept {
            if (!Built) {
                build();
            }
        }

        TextBuilder(const TextBuilder &) = delete;
        TextBuilder &operator=(const TextBuilder &) = delete;

        TextBuilder &color(Pixel value) noexcept {
            Pixel = value;
            return *this;
        }

        TextBuilder &font(FontId value) noexcept {
            Font = value;
            return *this;
        }

        TextBuilder &maxLineWidth(float value) noexcept {
            MaxLineWidth = value;
            return *this;
        }

        TextBuilder &width(float value) noexcept {
            Width = value;
            return *this;
        }

        TextBuilder &alpha(float value) noexcept {
            Pixel = Pixel.withAlpha(value);
            return *this;
        }

        Rect rect() noexcept {
            if (!Built) {
                build();
            }
            return rect_;
        }

        void build() noexcept {
            if (Built) {
                return;
            }
            Built = true;

            Vector2 size = ui.ctx().calculateTextSize(Text, Font, MaxLineWidth);
            float width = MaxLineWidth > 0.0f ? MaxLineWidth : size.x;
            if (Width > 0.0f) {
                width = Width;
            }
            size.x = width;

            rect_ = ui.ctx().placeItem(size);
            if (ui.ctx().isItemVisible(rect_)) {
                ui.ctx().renderText(rect_.position(), Text, Pixel, Font, MaxLineWidth);
            }
        }

      private:
        Ui &ui;
        u8StringView Text;
        Pixel Pixel;
        FontId Font;
        float MaxLineWidth = 0.0f;
        float Width = 0.0f;
        bool Built = false;
        Rect rect_;
    };

    // toggle row（切换控件行）中各个布局元素的排列顺序：
    // 指示器、标签，以及在控件具有附加元素时显示的附属元素，
    // 例如键帽样式的按键提示。
    // 附属元素始终排列在最后，位于整行的边缘，
    // 避免它与用于切换控件状态的点击区域发生冲突。
    enum class LayoutDirection : std::uint8_t {
        // 指示器在前，标签在后。
        // 例如复选框、单选按钮和开关的常见布局。
        Normal,
        // 标签在前，指示器在后。
        Reverse,
    };

    // 决定切换控件行中的各个布局元素如何分布在整行的可用宽度内。
    // 该枚举的命名参考了 CSS 的 justify-content 属性，因为两者表达的是同一个概念：
    // LayoutDirection 决定元素的排列顺序，而 HorizontalAlignment 决定如何利用可用空间。
    enum class HorizontalAlignment : std::uint8_t {
        // 整行的宽度仅与其内部布局元素所需的宽度相同，并靠在线的起始端。
        // 换句话说，切换控件只占用自身所需的空间。
        // 这是默认布局方式。
        Start,
        // 整行占满所在行提供的可用宽度，内部布局元素靠在整行的末端。
        End,
        // 整行占满所在行提供的可用宽度。
        // 第一个布局元素位于起始端，最后一个布局元素位于末端，
        // 两者之间剩余的可用空间由它们共同分隔，即将剩余空间全部放在元素之间。
        SpaceBetween,
    };

    // 描述切换控件行需要包含哪些元素，以及它应当如何利用所在行的可用空间。
    // 使用具名字段而不是参数列表，是因为使用该结构体的不同控件可能包含不同的布局元素，
    // 而且，如果调用时传入五个带默认值的参数，那么仅从调用代码本身很难看出每个参数的含义。
    struct ToggleRowParams {
        Vector2 markerSize;

        // 布局元素的排列方向，默认为正常顺序。
        // Normal：指示器在前，标签在后。
        // Reverse：标签在前，指示器在后。
        LayoutDirection direction = LayoutDirection::Normal;
        HorizontalAlignment alignment = HorizontalAlignment::Start;
        float spacing = -1.0f;
        // 限制扩展布局行可以占据的最大宽度。
        // 当布局行仅包裹自身元素、只占用元素所需宽度时，此参数不生效。
        float maxWidth = 0.0f;
        bool disabled = false;
    };

    // 复选框和单选按钮共用的布局结果：
    // 一行中包含一个状态指示器和一个可选标签，并且点击交互已经完成判定。
    // 具体控件负责维护自己的状态并绘制自己的指示器，
    // 因此这里采用普通的共享代码，而不是通过基类实现代码复用。
    struct ToggleLine {
        Rect rect;
        Vector2 textSize;
        Vector2 markerSize;
        Vector2 labelPosition;
        Vector2 markerPosition;
        InteractionResult interaction;
    };

    // 第二个 detail 块：切换行有自己的类型（ToggleRowParams / ToggleLine），所以单独开一块，
    // 紧挨着使用它们的复选框、单选按钮和开关。
    namespace detail {

        // 计算并放置切换控件行中的布局元素，执行点击交互判定，并返回交互状态。
        // indicatorSize 表示状态指示器的宽度和高度，例如复选框的方框或单选按钮的圆形区域。
        inline auto placeToggleLine(Context &ctx, Id id, u8StringView text, const ToggleRowParams &params) noexcept
            -> ToggleLine {
            const Style &style = ctx.style();
            const float gap = params.spacing >= 0.0f ? params.spacing : style.itemInnerSpacing.x;
            const bool hasText = !text.empty();

            ToggleLine line;
            line.markerSize = params.markerSize;
            line.textSize = hasText ? ctx.calculateTextSize(text) : Vector2();

            // 描述参与布局的元素。
            //
            // 每个 Slot 保存：
            //   size     - 当前元素的尺寸。
            //   position - 指向布局结果中对应位置字段的指针。
            //
            // 通过指针，可以统一计算不同元素的位置，
            // 而不必为指示器和标签分别编写布局代码。
            struct Slot {
                Vector2 size;
                Vector2 *position;
            };
            // 最多预留三个布局位置。
            // 当前实现实际添加的只有指示器和标签；
            // 如果将来需要支持快捷键提示等附属元素，可以继续添加对应的 Slot。
            Slot slots[3];
            // 当前实际参与布局的元素数量。
            std::size_t slotCount = 0;

            // 将具有有效宽度的元素加入布局列表。
            //
            // size.x <= 0 的元素不会被加入布局，因此不会占据水平空间，
            // 也不会参与后续的间距和位置计算。
            const auto addSlot = [&](Vector2 size, Vector2 &position) {
                if (size.x > 0.0f) {
                    slots[slotCount] = Slot{size, &position};
                    slotCount += 1;
                }
            };
            addSlot(params.markerSize, line.markerPosition);
            addSlot(line.textSize, line.labelPosition);

            // 如果要求反向排列，并且至少存在两个布局元素，
            // 则交换前两个 Slot。
            //
            // 交换的是布局列表中的元素，而不是直接交换尺寸或位置字段。
            // Slot 内部保存的位置指针仍然指向各自元素对应的结果字段。
            if (params.direction == LayoutDirection::Reverse && slotCount >= 2) {
                const Slot first = slots[0];
                slots[0] = slots[1];
                slots[1] = first;
            }
            float contentWidth = 0.0f;
            float contentHeight = 0.0f;
            for (std::size_t index = 0; index < slotCount; index += 1) {
                // 累加当前元素的宽度。
                // 第一个元素之前不需要间距，后续元素之前都需要间距。
                contentWidth += slots[index].size.x + (index > 0 ? gap : 0.0f);
                // 整行内容的高度取所有元素高度中的最大值，
                // 以便容纳其中最高的元素。
                contentHeight = maximum(contentHeight, slots[index].size.y);
            }

            // Start 模式只需要容纳自身元素。
            // End 和 SpaceBetween 则需要尽可能利用所在行提供的可用宽度，
            // 才能将元素放到末端或在元素之间分配额外空间。
            //
            // 这里获取的是当前内容区域的可用宽度。
            const float available = ctx.contentRegionWidth();
            float rowWidth = contentWidth;

            // 对于非 Start 布局，如果可用宽度大于内容自身所需的宽度，
            // 则将整行扩展到当前内容区域的可用宽度。
            if (params.alignment != HorizontalAlignment::Start && available > contentWidth) {
                rowWidth = available;
            }
            if (params.maxWidth > 0.0f && rowWidth > params.maxWidth) {
                rowWidth = maximum(params.maxWidth, contentWidth);
            }
            line.rect = ctx.placeItem(Vector2(rowWidth, contentHeight + style.framePadding.y));

            const float contentY = line.rect.y + style.framePadding.y * 0.5f;

            // 垂直居中排列所有元素。
            //
            // 每个元素的 Y 坐标 = 内容起始 Y 坐标
            //                   + (整行内容高度 - 当前元素高度) / 2。
            //
            // 这样即使标签和指示器的高度不同，
            // 它们也能在同一行中保持垂直居中。
            for (std::size_t index = 0; index < slotCount; index += 1) {
                slots[index].position->y = contentY + (contentHeight - slots[index].size.y) * 0.5f;
            }

            if (params.alignment == HorizontalAlignment::End) {
                float cursor = line.rect.maxX();
                for (std::size_t index = slotCount; index > 0; index -= 1) {
                    // 当前元素的右边缘与 cursor 对齐。
                    slots[index - 1].position->x = cursor - slots[index - 1].size.x;
                    // 向左移动一个元素的宽度及相应间距，
                    // 为前一个元素预留位置。
                    cursor -= slots[index - 1].size.x + gap;
                }
            } else {
                float slotGap = gap;
                if (params.alignment == HorizontalAlignment::SpaceBetween && slotCount > 1) {
                    float slotWidth = 0.0f;
                    for (std::size_t index = 0; index < slotCount; ++index) {
                        slotWidth += slots[index].size.x;
                    }
                    // 将剩余宽度平均分配到相邻元素之间。
                    //
                    // rowWidth - slotWidth：
                    //   整行宽度减去元素本身占用的总宽度。
                    //
                    // slotCount - 1：
                    //   N 个元素之间共有 N - 1 个间隔。
                    //
                    // 最终间距不会小于样式或调用方指定的 gap。
                    slotGap = maximum(gap, (rowWidth - slotWidth) / static_cast<float>(slotCount - 1));
                }
                // 从左向右依次放置元素。
                float cursor = line.rect.x;
                for (std::size_t index = 0; index < slotCount; ++index) {
                    // 将当前元素的左边缘放在 cursor 处。
                    slots[index].position->x = cursor;
                    // 移动到下一个元素的起始位置。
                    cursor += slots[index].size.x + slotGap;
                }
            }

            // 确定整行中哪些区域属于可点击区域。
            //
            // SpaceBetween 会将元素分散到整行的不同位置，
            // 因此元素之间的剩余空间也属于该控件。
            //
            // Start 和 End 则采用紧凑的点击区域，
            // 点击区域从第一个实际布局元素开始。
            float clickLeft = line.rect.x;
            if (params.alignment != HorizontalAlignment::SpaceBetween && slotCount > 0) {
                clickLeft = slots[0].position->x;
            }
            const Rect clickRect(clickLeft, line.rect.y, line.rect.maxX() - clickLeft, line.rect.height);
            line.interaction = ctx.clickBehavior(clickRect, id, !params.disabled);
            return line;
        }
    } // namespace detail

    struct CheckboxBuilder {

        CheckboxBuilder(Ui &ui, u8StringView text) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)) {}

        ~CheckboxBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }

        CheckboxBuilder(const CheckboxBuilder &) = delete;
        CheckboxBuilder &operator=(const CheckboxBuilder &) = delete;

        auto &&checked(this auto &&self, bool *value) noexcept {
            self.Value = value;
            return std::forward<decltype(self)>(self);
        }

        // 这一行中各元素的排列顺序：默认先放复选框，也可以先放标签。
        auto &&direction(this auto &&self, LayoutDirection value) noexcept {
            self.Direction = value;
            return std::forward<decltype(self)>(self);
        }

        // 控件这一行如何利用可用宽度：
        // 默认将各个元素紧凑地排列在行首，也可以将它们放到行尾，
        // 或者让它们沿着整行的宽度分散排列。
        auto &&alignment(this auto &&self, HorizontalAlignment value) noexcept {
            self.Alignment = value;
            return std::forward<decltype(self)>(self);
        }
        auto value(this auto &&self) noexcept {
            if (!self.IsBuilt) {
                std::forward<decltype(self)>(self).build();
            }
            return self.Value != nullptr && *self.Value;
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const float boxSize = style.fontSize * 0.95f;
            const ToggleLine line = detail::placeToggleLine(ui.ctx(), Id, Text,
                                                            ToggleRowParams{.markerSize = Vector2(boxSize, boxSize),
                                                                            .direction = Direction,
                                                                            .alignment = Alignment,
                                                                            .disabled = Disabled});

            if (line.interaction.clicked && Value != nullptr && !Disabled) {
                *Value = !*Value;
            }

            if (ui.ctx().isItemVisible(line.rect)) {
                // 用 placeToggleLine() 算出来的 markerPosition，而不是行左边缘：direction(Reverse)
                // 和 alignment(End/SpaceBetween) 下指示器并不在 line.rect.x 上，写死左边缘会把方框
                // 画到标签上面去。
                const Rect box(line.markerPosition, Vector2(boxSize, boxSize));
                const Pixel fill = line.interaction.held
                                       ? style.pixel(PixelIndex::FrameBackgroundActive)
                                       : (line.interaction.hovered ? style.pixel(PixelIndex::FrameBackgroundHovered)
                                                                   : style.pixel(PixelIndex::FrameBackground));
                detail::renderFrame(ui.ctx(), box, fill, style.pixel(PixelIndex::Border), style.frameRounding,
                                    style.frameBorderSize > 0.0f);
                if (Value != nullptr && *Value) {
                    detail::renderCheckMark(ui.ctx(), box,
                                            Disabled ? style.pixel(PixelIndex::TextDisabled)
                                                     : style.pixel(PixelIndex::CheckMark));
                }
                if (!Text.empty()) {
                    ui.ctx().renderText(line.labelPosition, Text,
                                        Disabled ? style.pixel(PixelIndex::TextDisabled)
                                                 : style.pixel(PixelIndex::Text),
                                        ui.ctx().font(), 0.0f);
                }
            }
        }

      private:
        Ui &ui;
        Id Id;
        u8StringView Text;
        bool *Value = nullptr;
        LayoutDirection Direction = LayoutDirection::Normal;
        HorizontalAlignment Alignment = HorizontalAlignment::Start;
        bool Disabled = false;
        bool IsBuilt = false;
    };

    class RadioButtonBuilder {
      public:
        RadioButtonBuilder(Ui &ui, u8StringView text, int optionValue) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)), OptionValue(optionValue) {}

        ~RadioButtonBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }

        RadioButtonBuilder(const RadioButtonBuilder &) = delete;
        RadioButtonBuilder &operator=(const RadioButtonBuilder &) = delete;

        RadioButtonBuilder &selected(int *current) noexcept {
            Current = current;
            return *this;
        }

        auto &&direction(this auto &&self, LayoutDirection value) noexcept {
            self.Direction = value;
            return std::forward<decltype(self)>(self);
        }

        auto &&alignment(this auto &&self, HorizontalAlignment value) noexcept {
            self.Alignment = value;
            return std::forward<decltype(self)>(self);
        }

        auto &&optionValue(this auto &&self, int value) noexcept {
            self.OptionValue = value;
            return std::forward<decltype(self)>(self);
        }

        auto selected() noexcept -> bool {
            if (!IsBuilt) {
                build();
            }
            return Current != nullptr && *Current == OptionValue;
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const float circleSize = style.fontSize * 0.9f;
            const ToggleLine line =
                detail::placeToggleLine(ui.ctx(), Id, Text,
                                        ToggleRowParams{.markerSize = Vector2(circleSize, circleSize),
                                                        .direction = Direction,
                                                        .alignment = Alignment,
                                                        .disabled = Disabled});

            if (line.interaction.clicked && Current != nullptr && !Disabled && *Current != OptionValue) {
                *Current = OptionValue;
            }

            if (ui.ctx().isItemVisible(line.rect)) {
                // 同上：圆心取 markerPosition，别用行左边缘。
                const Vector2 center(line.markerPosition.x + circleSize * 0.5f,
                                     line.markerPosition.y + circleSize * 0.5f);
                ui.ctx().drawList().addCircleFilled(
                    center, circleSize * 0.5f,
                    line.interaction.held ? style.pixel(PixelIndex::FrameBackgroundActive)
                                          : (line.interaction.hovered ? style.pixel(PixelIndex::FrameBackgroundHovered)
                                                                      : style.pixel(PixelIndex::FrameBackground)));
                ui.ctx().drawList().addCircle(center, circleSize * 0.5f, style.pixel(PixelIndex::Border), 0, 1.0f);
                if (Current != nullptr && *Current == OptionValue) {
                    ui.ctx().drawList().addCircleFilled(center, circleSize * 0.28f, style.pixel(PixelIndex::CheckMark));
                }
                if (!Text.empty()) {
                    ui.ctx().renderText(line.labelPosition, Text,
                                        Disabled ? style.pixel(PixelIndex::TextDisabled)
                                                 : style.pixel(PixelIndex::Text),
                                        ui.ctx().font(), 0.0f);
                }
            }
        }

      private:
        Ui &ui;
        Id Id;
        u8StringView Text;
        int *Current = nullptr;
        int OptionValue = 0;
        LayoutDirection Direction = LayoutDirection::Normal;
        HorizontalAlignment Alignment = HorizontalAlignment::Start;
        bool Disabled = false;
        bool IsBuilt = false;
    };

    class SwitchToggleBuilder {
      public:
        SwitchToggleBuilder(Ui &ui, u8StringView text)
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)) {}
        ~SwitchToggleBuilder() {
            if (!IsBuilt) {
                build();
            }
        }

        auto &&checked(this auto &&self, bool *value) noexcept {
            self.Value = value;
            return std::forward<decltype(self)>(self);
        }

        // 滑动动画的时间常数，单位为秒。
        // 数值越小，动画响应越快；设为 0 则立即到达目标位置，不播放渐变动画。
        auto &&smoothing(this auto &&self, float seconds) noexcept {
            self.Smoothing = seconds;
            return std::forward<decltype(self)>(self);
        }
        // 控件这一行中各元素的排列顺序：默认先放开关，也可以先放标签。
        auto &&direction(this auto &&self, LayoutDirection value) noexcept {
            self.Direction = value;
            return std::forward<decltype(self)>(self);
        }

        // 控件这一行如何利用可用的水平空间。
        // Start 让开关和标签紧挨着排列，整体从行首开始。
        // End 和 SpaceBetween 则会利用整行提供的宽度，
        // 让开关靠近行的另一端，并与标签拉开距离。
        // 如果想做出“左边是标签，右边是开关”的设置项布局，
        // 就应该使用这类对齐方式。
        auto &&alignment(this auto &&self, HorizontalAlignment value) noexcept {
            self.Alignment = value;
            return std::forward<decltype(self)>(self);
        }

      private:
        void build() {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const float trackWidth = style.fontSize * 1.8f;
            const float trackHeight = style.fontSize;

            const auto line = detail::placeToggleLine(ui.ctx(), Id, Text,
                                                      ToggleRowParams{.markerSize = Vector2(trackWidth, trackHeight),
                                                                      .direction = Direction,
                                                                      .alignment = Alignment,
                                                                      .disabled = Disabled});

            if (line.interaction.clicked && Value != nullptr && !Disabled) {
                *Value = !*Value;
            }
            const float target = (Value != nullptr && *Value) ? 1.0f : 0.0f;
            float slide = ui.animate(Id, target, Smoothing);

            if (!ui.ctx().isItemVisible(line.rect)) {
                return;
            }

            const Rect track(line.markerPosition.x, line.markerPosition.y, trackWidth, trackHeight);
            const float radius = trackHeight * 0.5f;
            const Pixel off = line.interaction.hovered ? style.pixel(PixelIndex::FrameBackgroundHovered)
                                                       : style.pixel(PixelIndex::FrameBackground);
            const Pixel on =
                line.interaction.held ? style.pixel(PixelIndex::ButtonActive) : style.pixel(PixelIndex::Button);
            Pixel fill = off.lerp(on, slide);
            if (Disabled) {
                fill = fill * 0.55f;
            }

            ui.ctx().drawList().addRectFilled(track.position(), track.maxPoint(), fill, radius, DrawCorner::All);
            if (style.frameBorderSize > 0.0f) {
                ui.ctx().drawList().addRect(track.position(), track.maxPoint(), style.pixel(PixelIndex::Border), radius,
                                            DrawCorner::All, style.frameBorderSize);
            }

            const float knobRadius = maximum(1.0f, radius - style.frameBorderSize - 1.0f);
            const float knobX = track.x + radius + (trackWidth - trackHeight) * slide;
            const Pixel knob = style.pixel(PixelIndex::SwitchToggleBlock);
            ui.ctx().drawList().addCircleFilled(Vector2(knobX, track.center().y), knobRadius,
                                                Disabled ? knob * 0.55f : knob);
            if (!Text.empty()) {
                ui.ctx().renderText(line.labelPosition, Text,
                                    Disabled ? style.pixel(PixelIndex::TextDisabled) : style.pixel(PixelIndex::Text),
                                    ui.ctx().font(), 0.0f);
            }
        }

      private:
        Ui &ui;
        Id Id;
        u8StringView Text;
        LayoutDirection Direction = LayoutDirection::Normal;
        HorizontalAlignment Alignment = HorizontalAlignment::Start;
        bool IsBuilt = false;
        float Smoothing = 0.08f;
        bool Disabled = false;
        bool *Value = nullptr;
    };

    // 当快捷键绑定尚未分配按键时，显示一个小型的键盘标记。
    //
    // 对于尚未绑定按键的快捷键，与其直接显示 "None"（无），
    // 不如用一个图形表示“这里可以放置一个按键”。
    // 该图形的外形表明它是一个按键占位符，而较小的尺寸则表示该位置当前为空。

    // width 表示图标的完整宽度。
    // 图标的高度以及内部按键标记的尺寸和布局都根据该宽度计算得出，
    // 因此无论将图标绘制成输入框大小，还是缩放成键帽大小，
    // 它都能保持一致的视觉效果和辨识度。
    //
    // 该绘制逻辑放在 detail 层，而不是放在具体控件中，
    // 是因为弹出窗口（Popup）可能会在当前帧结束时重新提交或绘制其几何图形。
    // 到那个阶段，能够访问的可能只剩下绘制列表（Draw List），
    // 因此需要将绘制逻辑放在能够直接操作绘制列表的位置。
    namespace detail {

        inline void drawKeyboardIcon(DrawList &list, Vector2 center, float width, Pixel color) noexcept {
            const float bodyWidth = maximum(6.0f, width);
            const float bodyHeight = bodyWidth * 0.62f;
            const Rect body(center.x - bodyWidth * 0.5f, center.y - bodyHeight * 0.5f, bodyWidth, bodyHeight);
            list.addRect(body.position(), body.maxPoint(), color, bodyHeight * 0.22f, DrawCorner::All,
                         maximum(1.0f, bodyWidth * 0.07f));

            // Three keys and a space bar: the least a keyboard shape needs to be read as one.
            const float keyWidth = maximum(1.0f, bodyWidth * 0.14f);
            const float keyHeight = maximum(1.0f, bodyHeight * 0.18f);
            const float keyY = body.y + bodyHeight * 0.3f - keyHeight * 0.5f;
            for (int index = -1; index <= 1; ++index) {
                const float keyX = center.x + static_cast<float>(index) * (bodyWidth * 0.22f) - keyWidth * 0.5f;
                list.addRectFilled(Vector2(keyX, keyY), Vector2(keyX + keyWidth, keyY + keyHeight), color,
                                   keyHeight * 0.3f);
            }

            const float barWidth = bodyWidth * 0.42f;
            const float barHeight = maximum(1.0f, bodyHeight * 0.16f);
            const float barY = body.y + bodyHeight * 0.68f - barHeight * 0.5f;
            list.addRectFilled(Vector2(center.x - barWidth * 0.5f, barY),
                               Vector2(center.x + barWidth * 0.5f, barY + barHeight), color, barHeight * 0.4f);
        }

    } // namespace detail

    // 用于显示按键绑定的字段。
    // 该字段会显示已绑定按键的名称；如果尚未绑定按键，则显示键盘标记。
    //
    // 点击该字段后，会在字段下方打开一个小型弹出窗口，
    // 窗口中显示文字 "Key" 和一个按钮。
    // 按下该按钮后，控件开始捕获用户接下来按下的按键。
    //
    // 捕获过程中：
    //   - 按下 Escape：停止捕获，但保留原有的按键绑定。
    //   - 按下 Backspace 或 Delete：清除当前按键绑定。
    //   - 点击弹出窗口以外的任意位置：关闭弹出窗口。
    //
    // 在捕获按键期间，该字段会接管键盘输入。
    // 宿主程序可以通过调用 wantsKeyboardCapture()，
    // 判断当前是否应当由 UI 接收键盘输入，从而避免宿主程序自己的快捷键
    // 在用户配置按键时意外触发。

    // 按键绑定的具体用途由宿主程序负责处理。
    //
    // 例如：
    //   ui.keyBind("Jump").key(&jumpKey);
    //
    //   if (ctx.io().isKeyPressed(jumpKey)) {
    //       jump();
    //   }
    //
    // 上述代码将一个名为 "Jump" 的按键绑定控件与 jumpKey 关联。
    // 当用户通过 UI 设置按键后，宿主程序可以检查该按键是否被按下，
    // 并在按下时执行 jump()。
    class KeyBindBuilder {
      public:
        KeyBindBuilder(Ui &ui, u8StringView text, PopupState &popup, KeyBindState &keybind) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)), Popup(popup), Keybind(keybind) {}

        ~KeyBindBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }

        KeyBindBuilder(const KeyBindBuilder &) = delete;
        KeyBindBuilder &operator=(const KeyBindBuilder &) = delete;

        // 将该控件绑定到调用方提供的按键变量。
        // 按键状态由宿主代码持有和管理：控件每帧读取该变量，
        // 只有当用户重新绑定按键时，控件才会将新按键写回该变量。
        auto &&key(this auto &&self, Input::KeyCode *target) noexcept {
            self.Key = target;
            return std::forward<decltype(self)>(self);
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const float iconWidth = style.fontSize;
            const float height = style.fontSize + style.framePadding.y * 2.0f;

            // 按键绑定的实际存储由调用方负责。
            //
            // 当前控件会读取现有的绑定值，并将其交给弹出窗口的行为逻辑；
            // 行为逻辑再返回当前帧结束后应该采用的绑定值。
            //
            // 点击这个触发区域只负责打开弹出窗口；
            // 真正开始捕获键盘输入的操作，由弹出窗口内部的按钮负责。
            //
            // 如果调用方提供了 Key 指针，就从指针指向的位置读取当前绑定；
            // 否则使用控件内部保存的 Bound 作为当前绑定值。
            const Input::KeyCode current = Key != nullptr ? *Key : Bound;

            // 标签和触发区排在一行：标签在左，触发区在右。
            //
            // 触发区至少要放得下它要居中显示的键名——原来是固定 fontSize 宽（约 20px），
            // 而 "Space" 这类键名量出来有五六十像素，名字会直接溢到标签上面去。
            // 宽度按"当前"绑定算：本帧刚改绑的话，文字要下一帧才和新宽度对齐（和原来显示
            // 新键名的时序一致）。
            const Vector2 labelSize = Text.empty() ? Vector2() : ui.ctx().calculateTextSize(Text);
            const float gap = Text.empty() ? 0.0f : style.itemInnerSpacing.x;
            const u8StringView currentName = keyBindText(current, false);
            const float triggerWidth =
                current == Input::KeyCode::None
                    ? iconWidth
                    : maximum(iconWidth, ui.ctx().calculateTextSize(currentName).x + style.framePadding.x * 2.0f);
            rect_ = ui.ctx().placeItem(Vector2(labelSize.x + gap + triggerWidth, maximum(height, labelSize.y)));
            const Rect trigger(rect_.maxX() - triggerWidth, rect_.y + (rect_.height - height) * 0.5f, triggerWidth,
                               height);

            // 处理按键绑定弹出窗口的状态与交互。
            //   Id       - 控件的唯一标识符，用于关联弹出窗口状态。
            //   trigger  - 触发区域的位置和尺寸。
            //   current  - 当前绑定的按键。
            const KeyBindPopupState popup = keyBindPopup(Id, trigger, current, !Disabled);
            const KeyBindResult &bind = popup.result;
            popup_ = popup.panel;
            changed_ = bind.changed;
            Clicked = bind.interaction.clicked;
            Listening = bind.listening;
            Bound = bind.key;
            if (bind.changed && Key != nullptr) {
                *Key = bind.key;
            }

            if (!ui.ctx().isItemVisible(rect_)) {
                return;
            }

            // 标签画在行的左端，垂直居中。
            if (!Text.empty()) {
                ui.ctx().renderText(Vector2(rect_.x, rect_.y + (rect_.height - labelSize.y) * 0.5f), Text,
                                    Disabled ? style.pixel(PixelIndex::TextDisabled) : style.pixel(PixelIndex::Text),
                                    ui.ctx().font(), 0.0f);
            }

            Pixel ink = style.pixel(PixelIndex::Text);
            if (Disabled) {
                ink = style.pixel(PixelIndex::TextDisabled);
            } else if (bind.listening || bind.interaction.held) {
                ink = style.pixel(PixelIndex::ButtonActive);
            } else if (bind.interaction.hovered) {
                ink = style.pixel(PixelIndex::ButtonHovered);
            }

            // 根据当前最终的按键绑定，决定触发区域中显示什么内容。
            //
            // 尚未绑定按键时，显示键盘标记；
            // 已经绑定按键时，显示对应的按键名称。
            //
            // 注意，这里读取的是弹出窗口处理之后的 Bound，
            // 而不是布局阶段使用的初始绑定值 current。
            //
            // 因此，如果本帧修改了按键绑定，触发区域就可以立即显示新按键的名称。
            //
            // 按键捕获期间的提示文字由弹出窗口内部的按钮显示。
            // 因为用户此时正在与那个按钮交互，所以提示文字应当显示在那里，
            // 而不是显示在这个触发区域中。
            const u8StringView text = keyBindText(Bound, false);
            if (Bound == Input::KeyCode::None) {
                detail::drawKeyboardIcon(ui.ctx().drawList(), trigger.center(), iconWidth, ink);
            } else {
                const Vector2 textSize = ui.ctx().calculateTextSize(text);
                ui.ctx().renderText(trigger.center() - textSize * 0.5f, text, ink, ui.ctx().font(), 0.0f);
            }
        }

        bool clicked() noexcept {
            if (!IsBuilt) {
                build();
            }
            return Clicked;
        }

        bool listening() noexcept {
            if (!IsBuilt) {
                build();
            }
            return Listening;
        }

        Input::KeyCode key() noexcept {
            if (!IsBuilt) {
                build();
            }
            return Bound;
        }

        Rect popupRect() noexcept {
            if (!IsBuilt) {
                build();
            }
            return popup_;
        }

      private:
        // 记录弹出窗口在当前帧中的处理结果：
        // 包括内部按钮对输入的处理结果、面板最终的位置，
        // 以及弹出窗口当前是否处于打开状态。
        struct KeyBindPopupState {
            KeyBindResult result;
            // 弹出窗口面板的矩形区域，包含面板的位置和尺寸。
            // 当弹出窗口关闭时，该矩形区域为空。
            Rect panel;
            bool open = false;
        };

        // 按键绑定控件在已绑定或等待绑定时显示的内容：按键名称或提示文字。
        // 未绑定的控件则显示键盘图标，详见 detail::drawKeyboardIcon()。
        inline auto keyBindText(Input::KeyCode key, bool listening) noexcept -> u8StringView {
            if (listening) {
                return u8"press a key";
            }
            return Input::keyCodeName(key);
        }

        // 按键绑定控件打开的弹出面板：它是一个位于触发控件下方的小面板，包含文字 "Key" 和一个按钮。
        // 按下按钮后开始监听键盘输入，用户接下来按下的按键就会成为绑定的按键。
        // 面板会作为覆盖层排入绘制队列，因此它会绘制在触发控件之后声明的所有内容之上。
        // 面板打开期间，其余 UI 不会接收到鼠标输入：点击面板外部的操作只用于关闭面板。
        auto keyBindPopup(Id id, const Rect &trigger, Input::KeyCode key, bool enabled) noexcept -> KeyBindPopupState {
            const Style &style = ui.ctx().style();
            const Vector2 mouse = ui.ctx().io().currentMousePosition();
            const bool wasOpen = isPopupOpen(id);
            const bool wasListening = isKeyBindListening(id);

            // 按钮需要容纳提示文字和任意按键名称，因此面板的宽度应根据它们中最长的文字来确定，
            // 而不是根据当前恰好绑定的按键名称来确定。
            const u8StringView label = u8"Key";
            const Vector2 labelSize = ui.ctx().calculateTextSize(label);
            const Vector2 promptSize = ui.ctx().calculateTextSize(keyBindText(Input::KeyCode::None, true));
            const Vector2 unboundSize = ui.ctx().calculateTextSize(keyBindText(Input::KeyCode::None, false));
            const Vector2 longestName = ui.ctx().calculateTextSize(keyBindText(Input::KeyCode::Menu, false));
            const float buttonWidth =
                maximum(promptSize.x, maximum(unboundSize.x, longestName.x)) + style.framePadding.x * 2.0f;
            const float buttonHeight =
                maximum(promptSize.y, maximum(unboundSize.y, longestName.y)) + style.framePadding.y;
            const float panelWidth = style.framePadding.x * 2.0f + labelSize.x + style.itemInnerSpacing.x + buttonWidth;
            const float panelHeight = maximum(buttonHeight, labelSize.y) + style.framePadding.y * 2.0f;

            Rect panel(trigger.x, trigger.maxY() + style.itemInnerSpacing.y, panelWidth, panelHeight);

            // 面板必须位于当前上下文可见且可接收鼠标输入的范围内。
            // 覆盖层本身不会受到裁剪，但位于裁剪矩形之外的控件完全无法响应悬停，
            // 因此，如果面板超出窗口边缘，就会出现面板能够绘制出来、却无法交互的情况。
            const Rect bounds = ui.ctx().drawList().currentClipRect();
            if (panel.maxX() > bounds.maxX()) {
                panel.x = maximum(bounds.x, bounds.maxX() - panel.width);
            }
            if (panel.maxY() > bounds.maxY()) {
                // 下方没有足够的空间时，将面板翻转到触发控件上方，
                // 就像菜单靠近屏幕底部边缘时的处理方式一样。
                panel.y = maximum(bounds.y, trigger.y - style.itemInnerSpacing.y - panel.height);
            }
            const Rect button(panel.maxX() - style.framePadding.x - buttonWidth,
                              panel.y + (panel.height - buttonHeight) * 0.5f, buttonWidth, buttonHeight);

            // 点击面板外部时关闭面板，而且这次点击也不会传递给其余 UI。
            // 上下文会将打开的弹出面板视为鼠标模态区域。
            bool open = wasOpen;
            if (enabled && ui.ctx().io().isMouseClicked(Input::MouseButton::Left)) {
                if (wasOpen) {
                    open = panel.contains(mouse);
                } else {
                    open = trigger.contains(mouse);
                }
            }

            KeyBindPopupState state;
            state.result.key = key;
            KeyBindResult &result = state.result;
            if (open) {
                openPopup(id, panel);
                result = ui.keyBindBehavior(button, id, key, true);
                if (result.changed || (wasListening && !result.listening)) {
                    // 无论是刚刚完成按键绑定，还是用户按下 Escape 退出按键捕获，
                    // 都意味着面板已经没有继续显示的必要。
                    open = false;
                    closePopup();
                }
            } else if (wasOpen) {
                closePopup();
            }

            if (!open) {
                return state;
            }
            state.open = true;
            state.panel = panel;

            // 弹出面板在这里直接绘制到覆盖层中，该层会在所有窗口绘制完成后再进行绘制。
            // 面板的绘制顺序由覆盖层决定，窗口帧本身不需要了解弹出面板的外观或绘制细节。
            DrawList &layer = ui.ctx().overlay();
            layer.addRectFilled(panel.position(), panel.maxPoint(), style.pixel(PixelIndex::WindowBackground),
                                style.frameRounding);
            if (style.frameBorderSize > 0.0f) {
                layer.addRect(panel.position(), panel.maxPoint(), style.pixel(PixelIndex::Border), style.frameRounding,
                              DrawCorner::All, style.frameBorderSize);
            }
            const Rect labelBox(panel.x + style.framePadding.x, panel.y + (panel.height - labelSize.y) * 0.5f,
                                labelSize.x, labelSize.y);
            ui.ctx().renderTextToDrawList(layer, labelBox.position(), label, style.pixel(PixelIndex::Text),
                                          ui.ctx().font(), 0.0f);

            Pixel buttonFill = style.pixel(PixelIndex::Button);
            if (!enabled) {
                buttonFill = buttonFill * 0.55f;
            } else if (result.listening || result.interaction.held) {
                buttonFill = style.pixel(PixelIndex::ButtonActive);
            } else if (result.interaction.hovered) {
                buttonFill = style.pixel(PixelIndex::ButtonHovered);
            }
            layer.addRectFilled(button.position(), button.maxPoint(), buttonFill, style.frameRounding);
            if (style.frameBorderSize > 0.0f) {
                layer.addRect(button.position(), button.maxPoint(), style.pixel(PixelIndex::Border),
                              style.frameRounding, DrawCorner::All, style.frameBorderSize);
            }

            // 按钮始终显示某种文字：当前按键名称、输入提示，或说明按下按钮后会执行什么操作。
            // 因此，无论处于哪种状态，按钮都只需要通过文字来绘制，无需切换到其他绘制方式。
            const u8StringView value = keyBindText(result.key, result.listening);
            const Vector2 valueSize = ui.ctx().calculateTextSize(value);
            ui.ctx().renderTextToDrawList(layer, button.center() - valueSize * 0.5f, value,
                                          style.pixel(PixelIndex::Text), ui.ctx().font(), 0.0f);
            return state;
        }

        // 当弹出面板由 \p id 持有时返回 true。
        // 同一时间只能打开一个弹出面板，无论它由哪个控件打开；打开另一个面板会替换当前面板。
        auto isPopupOpen(Id id) noexcept -> bool { return id != InvalidId && Popup.id == id; }

        // 在 \p bounds 指定的区域打开由 \p id 标识的弹出面板，并记录其位置和范围，
        // 以便鼠标输入拦截逻辑判断哪些点击会关闭面板。
        // 同时记录当前帧编号，这样如果某个控件不再被声明，其弹出面板就不会遗留，
        void openPopup(Id id, const Rect &bounds) noexcept {
            Popup.id = id;
            Popup.bounds = bounds;
            Popup.frame = ui.ctx().frame();
        }

        void closePopup() noexcept {
            Popup.id = InvalidId;
            Popup.bounds = Rect();
        }

        // 当 \p id 正在等待用户按下按键时返回 true；参见 run()。
        auto isKeyBindListening(Id id) noexcept -> bool { return id != InvalidId && Keybind.listening == id; }

        Ui &ui;
        Id Id;
        Rect rect_;
        Rect popup_;
        u8StringView Text;
        Input::KeyCode *Key = nullptr;
        Input::KeyCode Bound = Input::KeyCode::None;
        PopupState &Popup;
        KeyBindState &Keybind;
        bool Disabled = false;
        bool IsBuilt = false;
        bool changed_ = false;
        bool Clicked = false;
        bool Listening = false;
    };

    // 滑块控件中拖动手柄的形状。
    enum class SliderGrabShape : std::uint8_t {
        // 圆形手柄，默认形状。
        Circle,
        // 矩形手柄，高度与滑块轨道一致。
        Rectangle,
    };

    template <typename T> class SliderBuilder {
      public:
        SliderBuilder(Ui &ui, u8StringView text) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)) {}

        ~SliderBuilder() noexcept {
            if (!Built) {
                build();
            }
        }

        SliderBuilder(const SliderBuilder &) = delete;
        SliderBuilder &operator=(const SliderBuilder &) = delete;

        void bind(T *target) noexcept { Value = target; }
        auto &&range(this auto &&self, const T &minimum, const T &maximum) noexcept {
            self.Minimum = minimum;
            self.Maximum = maximum;
            return std::forward<decltype(self)>(self);
        }

        /// 绘制较细的轨道，而不是绘制一条与整行等高的轨道。
        /// 此参数的转发方式与其他链式设置函数保持一致，因此可以直接对工厂函数返回的临时对象调用该函数；
        auto &&thin(this auto &&self) noexcept {
            self.Thin = true;
            return std::forward<decltype(self)>(self);
        }
        void grabShape(SliderGrabShape value) noexcept { GrabShape = value; }
        void valueOnLabel(bool enabled = true) noexcept { ValueOnLabel = enabled; }
        void disabled(bool value = true) noexcept { Disabled = value; }

        auto changed() noexcept -> bool {
            if (!Built) {
                build();
            }
            return Changed;
        }

        T value() noexcept {
            if (!Built) {
                build();
            }
            return Value != nullptr ? *Value : T{};
        }

        auto &&value(this auto &&self, T *value) noexcept {
            self.Value = value;
            return std::forward<decltype(self)>(self);
        }

        Rect rect() noexcept {
            if (!Built) {
                build();
            }
            return rect_;
        }

        void build() noexcept {
            if (Built) {
                return;
            }
            Built = true;

            const Style &style = ui.ctx().style();

            const Vector2 textSize = Text.empty() ? Vector2() : ui.ctx().calculateTextSize(Text);
            Rect labelRow;
            if (!Text.empty()) {
                // valueOnLabel() stretches the row over the whole line so the value
                // can sit against its right edge; otherwise it only takes the label.
                labelRow =
                    ui.ctx().placeItem(ValueOnLabel ? Vector2(ui.ctx().contentRegionWidth(), textSize.y) : textSize);
                // 这里不需要 newLine()：每个控件本来就独占一行，下面那次 placeItem 会自己换到下一行。
            }

            const float rowHeight = style.fontSize + style.framePadding.y * 2.0f;
            const float width = ui.ctx().contentRegionWidth();
            rect_ = ui.ctx().placeItem(Vector2(width, rowHeight));

            // 细滑块绘制的轨道，垂直居中于其所在的行内。该行
            // 保持原有的高度，因此数值文本和便于抓取的滑块手柄
            // 仍有足够的空间，同时拖动区域也更容易被命中。
            const float trackHeight = Thin ? maximum(1.0f, style.thinTrackThickness) : rowHeight;
            const Rect track(rect_.x, rect_.y + (rowHeight - trackHeight) * 0.5f, rect_.width, trackHeight);

            const float grabWidth = maximum(style.grabMinimumSize, rowHeight * 0.55f);
            const float initial = Value != nullptr ? toFloat(*Value) : toFloat(Minimum);
            const DragResult drag = ui.dragBehavior(rect_, Id, initial, toFloat(Minimum), toFloat(Maximum), grabWidth,
                                                    !Disabled && Value != nullptr);
            const bool dragging = drag.changed;
            const bool hovered = drag.hovered;

            T bound = fromFloat(drag.value);
            if (Maximum <= Minimum) {
                bound = Minimum;
            } else if (bound < Minimum) {
                bound = Minimum;
            } else if (bound > Maximum) {
                bound = Maximum;
            }
            if (Value != nullptr) {
                *Value = bound;
            }
            Changed = dragging;
            const auto valueText = detail::format(ui.arena(), u8"{}", formatArg(bound));
            const Vector2 valueSize = ui.ctx().calculateTextSize(valueText);
            const bool valueOnRow = ValueOnLabel && !Text.empty();

            // The label row is drawn once the value is known, so a drag updates the
            // number in the same frame that moves the grab.
            if (!Text.empty() && ui.ctx().isItemVisible(labelRow)) {
                ui.ctx().renderText(labelRow.position(), Text, style.pixel(PixelIndex::Text), ui.ctx().font(), 0.0f);
                if (valueOnRow) {
                    ui.ctx().renderText(
                        Vector2(labelRow.maxX() - valueSize.x, labelRow.y + (labelRow.height - valueSize.y) * 0.5f),
                        valueText, style.pixel(PixelIndex::Text), ui.ctx().font(), 0.0f);
                }
            }

            if (ui.ctx().isItemVisible(rect_)) {
                detail::renderFrame(ui.ctx(), track, style.pixel(PixelIndex::FrameBackground),
                                    style.pixel(PixelIndex::Border), style.frameRounding, style.frameBorderSize > 0.0f);

                const float span = toFloat(Maximum) - toFloat(Minimum);
                const float normalized =
                    span > 0.0f ? clampFloat((toFloat(bound) - toFloat(Minimum)) / span, 0.0f, 1.0f) : 0.0f;
                const float fillWidth = normalized * track.width;
                if (fillWidth > 0.5f) {
                    ui.ctx().drawList().addRectFilled(track.position(), Vector2(track.x + fillWidth, track.maxY()),
                                                      style.pixel(PixelIndex::SliderGrab), style.frameRounding);
                }

                const float grabX = track.x + normalized * (track.width - grabWidth) + grabWidth * 0.5f;
                const Pixel grabColor = (dragging || hovered) ? style.pixel(PixelIndex::SliderGrabActive)
                                                              : style.pixel(PixelIndex::SliderGrab);

                if (GrabShape == SliderGrabShape::Rectangle) {
                    const float inset = style.frameBorderSize;
                    const Rect grab(grabX - grabWidth * 0.5f, track.y + inset, grabWidth,
                                    maximum(1.0f, track.height - inset * 2.0f));
                    ui.ctx().drawList().addRectFilled(grab.position(), grab.maxPoint(), grabColor, style.frameRounding);
                } else {
                    ui.ctx().drawList().addCircleFilled(Vector2(grabX, track.center().y), grabWidth * 0.5f, grabColor);
                }

                if (!valueOnRow) {
                    ui.ctx().renderText(rect_.center() - valueSize * 0.5f, valueText, style.pixel(PixelIndex::Text),
                                        ui.ctx().font(), 0.0f);
                }
            }
        }

      private:
        static float toFloat(T value) noexcept { return static_cast<float>(value); }

        static T fromFloat(float value) noexcept {
            if constexpr (std::is_same_v<T, int>) {
                return static_cast<T>(std::round(value));
            } else {
                return static_cast<T>(value);
            }
        }

        /// What snprintf expects for T: %d takes an int, %f a double.
        static auto formatArg(T value) noexcept {
            if constexpr (std::is_same_v<T, int>) {
                return value;
            } else {
                return static_cast<double>(value);
            }
        }

        static constexpr T defaultMaximum() noexcept {
            if constexpr (std::is_same_v<T, int>) {
                return T{100};
            } else {
                return T{1};
            }
        }

        Ui &ui;
        Id Id;
        Rect rect_;
        u8StringView Text;
        T *Value = nullptr;
        T Minimum = T{};
        T Maximum = defaultMaximum();
        bool Thin = false;
        bool Disabled = false;
        bool Built = false;
        bool Changed = false;
        SliderGrabShape GrabShape = SliderGrabShape::Rectangle;
        bool ValueOnLabel = false;
    };

    struct TextInputBuilder {
        TextInputBuilder(Ui &ui, u8StringView text) noexcept
            : ui(ui), Id(ui.ctx().id(labelId(text))), Text(labelText(text)) {}

        ~TextInputBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }

        TextInputBuilder(const TextInputBuilder &) = delete;
        TextInputBuilder &operator=(const TextInputBuilder &) = delete;

        /// The caller's storage. It has to stay NUL terminated: the widget never
        /// writes past the last byte of the span.
        TextInputBuilder &buffer(Span<char8_t> buffer) noexcept {
            Buffer = buffer;
            return *this;
        }

        template <std::size_t N> TextInputBuilder &buffer(char (&array)[N]) noexcept {
            Buffer = Span<char8_t>(array, N);
            return *this;
        }

        TextInputBuilder &hint(u8StringView text) noexcept {
            Hint = text;
            return *this;
        }

        TextInputBuilder &width(float value) noexcept {
            explicitWidth_ = value;
            return *this;
        }

        u8StringView text() noexcept {
            if (!IsBuilt) {
                build();
            }
            return u8StringView(Buffer.data(), textLength(Buffer));
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            if (!Text.empty()) {
                const Vector2 labelSize = ui.ctx().calculateTextSize(Text);
                const Rect labelRect = ui.ctx().placeItem(labelSize);
                if (ui.ctx().isItemVisible(labelRect)) {
                    ui.ctx().renderText(labelRect.position(), Text, style.pixel(PixelIndex::Text), ui.ctx().font(),
                                        0.0f);
                }
                // 这里不需要 newLine()：标签自己就独占了一行，下面的输入框会换到下一行。
            }

            const float height = style.fontSize + style.framePadding.y * 2.0f;
            const float width = explicitWidth_ > 0.0f ? explicitWidth_ : ui.ctx().contentRegionWidth();
            const Rect rect = rect_ = ui.ctx().placeItem(Vector2(width, height));

            if (!disabled_ && !Buffer.empty()) {
                ui.textInputBehavior(rect, Id, Buffer);
            }

            const bool focused = Focused = ui.isTextFocused(Id);
            if (ui.ctx().isItemVisible(rect)) {
                const Pixel fill =
                    focused ? style.pixel(PixelIndex::FrameBackgroundActive) : style.pixel(PixelIndex::FrameBackground);
                detail::renderFrame(ui.ctx(), rect, fill, style.pixel(PixelIndex::Border), style.frameRounding,
                                    style.frameBorderSize > 0.0f);

                const u8StringView content = u8StringView(Buffer.data(), textLength(Buffer));
                const Vector2 textPosition =
                    rect.position() + Vector2(style.framePadding.x, (rect.height - style.fontSize) * 0.5f);
                if (content.empty()) {
                    if (!Hint.empty()) {
                        ui.ctx().renderText(textPosition, Hint, style.pixel(PixelIndex::TextDisabled), ui.ctx().font(),
                                            0.0f);
                    }
                } else {
                    ui.ctx().renderText(textPosition, content, style.pixel(PixelIndex::Text), ui.ctx().font(), 0.0f);
                }

                if (focused) {
                    const std::size_t caret = ui.caretPosition();
                    const std::size_t caretOffset = caret < content.size() ? caret : content.size();

                    const u8StringView caretText(content.data(), caretOffset);
                    const float caretX = textPosition.x + ui.ctx().calculateTextSize(caretText).x + 1.0f;
                    ui.ctx().drawList().addLine(Vector2(caretX, rect.y + style.framePadding.y * 0.5f),
                                                Vector2(caretX, rect.maxY() - style.framePadding.y * 0.5f),
                                                style.pixel(PixelIndex::Text), 1.0f);
                }
            }
        }

        /// Screen space rectangle the field was placed into.
        Rect rect() noexcept {
            if (!IsBuilt) {
                build();
            }
            return rect_;
        }

        /// The widget id, e.g. to correlate with editState().
        Id id() const noexcept { return Id; }

        /// True while the field owns the keyboard.
        bool focused() noexcept {
            if (!IsBuilt) {
                build();
            }
            return Focused;
        }

      private:
        Ui &ui;
        Id Id;
        Rect rect_;
        u8StringView Text;
        u8StringView Hint;
        Span<char8_t> Buffer;
        bool Focused = false;
        float explicitWidth_ = 0.0f;
        bool disabled_ = false;
        bool IsBuilt = false;
    };

    class ProgressBarBuilder {
      public:
        ProgressBarBuilder(Ui &ui) noexcept : ui(ui) {}

        ~ProgressBarBuilder() noexcept {
            if (!IsBuilt) {
                build();
            }
        }

        ProgressBarBuilder(const ProgressBarBuilder &) = delete;
        ProgressBarBuilder &operator=(const ProgressBarBuilder &) = delete;

        ProgressBarBuilder &fraction(float value) noexcept {
            Fraction = clampFloat(value, 0.0f, 1.0f);
            return *this;
        }

        ProgressBarBuilder &size(Vector2 value) noexcept {
            ExplicitSize = value;
            return *this;
        }

        ProgressBarBuilder &size(float width, float height) noexcept { return size(Vector2(width, height)); }

        ProgressBarBuilder &overlay(u8StringView text) noexcept {
            Overlay = text;
            return *this;
        }

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            const Style &style = ui.ctx().style();
            const float height = ExplicitSize.y > 0.0f ? ExplicitSize.y : style.fontSize + style.framePadding.y;
            const float width = ExplicitSize.x > 0.0f ? ExplicitSize.x : ui.ctx().contentRegionWidth();
            const Rect rect = rect_ = ui.ctx().placeItem(Vector2(width, height));

            if (ui.ctx().isItemVisible(rect)) {
                detail::renderFrame(ui.ctx(), rect, style.pixel(PixelIndex::FrameBackground),
                                    style.pixel(PixelIndex::Border), style.frameRounding, style.frameBorderSize > 0.0f);
                const float fillWidth = Fraction * rect.width;
                if (fillWidth > 0.5f) {
                    ui.ctx().drawList().addRectFilled(rect.position(), Vector2(rect.x + fillWidth, rect.maxY()),
                                                      style.pixel(PixelIndex::ProgressBar), style.frameRounding);
                }
                if (!Overlay.empty()) {
                    const Vector2 textSize = ui.ctx().calculateTextSize(Overlay);
                    ui.ctx().renderText(rect.center() - textSize * 0.5f, Overlay, style.pixel(PixelIndex::Text),
                                        ui.ctx().font(), 0.0f);
                }
            }
        }

        /// Screen space rectangle the bar was placed into.
        Rect rect() noexcept {
            if (!IsBuilt) {
                build();
            }
            return rect_;
        }

      private:
        Ui &ui;
        Rect rect_;
        u8StringView Text;
        u8StringView Overlay;
        Vector2 ExplicitSize;
        float Fraction = 0.0f;
        bool IsBuilt = false;
    };

    class SeparatorBuilder {
      public:
        SeparatorBuilder(Ui &ui) noexcept : ui(ui) {}
        ~SeparatorBuilder() {
            if (!IsBuilt) {
                build();
            }
        }
        SeparatorBuilder(const SeparatorBuilder &) = delete;
        SeparatorBuilder &operator=(const SeparatorBuilder &) = delete;

        void build() {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;

            auto &style = ui.ctx().style();
            const float thickness = maximum(1.0f, style.frameBorderSize);
            Rect = ui.ctx().placeItem(Vector2(ui.ctx().contentRegionWidth(), thickness));
            if (!ui.ctx().isItemVisible(Rect)) {
                return;
            }
            auto a = style.pixel(PixelIndex::Separator);
            const auto center = Rect.center();
            ui.ctx().drawList().addLine(Vector2(Rect.x, center.y), Vector2(Rect.maxX(), center.y), a, thickness);
        }

      private:
        Ui &ui;
        bool IsBuilt = false;
        Rect Rect;
    };

    class SeparatorTextBuilder {
      public:
        SeparatorTextBuilder(Ui &ui, u8StringView text) noexcept : ui(ui), Text(text) {}
        ~SeparatorTextBuilder() {
            if (!IsBuilt) {
                build();
            }
        }
        SeparatorTextBuilder(const SeparatorTextBuilder &) = delete;
        SeparatorTextBuilder &operator=(const SeparatorTextBuilder &) = delete;

        void build() noexcept {
            if (IsBuilt) {
                return;
            }
            IsBuilt = true;
            float textPosition = 30.f;

            auto &style = ui.ctx().style();
            const float thickness = maximum(1.0f, style.frameBorderSize);
            const Vector2 textSize = ui.ctx().calculateTextSize(Text);
            const float widgetWidth = maximum(textSize.x + textPosition, ui.ctx().contentRegionWidth());

            Rect = ui.ctx().placeItem(Vector2(widgetWidth, maximum(textSize.y, thickness) + style.framePadding.y));
            if (!ui.ctx().isItemVisible(Rect)) {
                return;
            }
            // 文字起点允许的最小/最大横坐标：控件本身保证文字放得下（widgetWidth >= textSize.x + 30），
            // 所以下面这个 clamp 目前永远不生效；留着是为了让"标签不越出控件"这条不变量写在代码里，
            // 万一以后 textPosition 变成可配置的（甚至比内容还宽）也不会画到控件外面。
            const float lowestX = Rect.x;
            const float highestX = maximum(Rect.x, Rect.maxX() - textSize.x);
            // 根据目标位置比例计算出来的理想文字起点。
            const float textX = clampFloat(Rect.x + textPosition, lowestX, highestX);

            const float centerY = Rect.y + Rect.height * 0.5f;
            const float gap = style.itemInnerSpacing.x;
            const float leadingEndX = textX - gap;
            const float trailingStartX = textX + textSize.x + gap;
            if (leadingEndX > Rect.x) {
                ui.ctx().drawList().addLine(Vector2(Rect.x, centerY), Vector2(leadingEndX, centerY),
                                            style.pixel(PixelIndex::Separator), thickness);
            }
            if (trailingStartX < Rect.maxX()) {
                ui.ctx().drawList().addLine(Vector2(trailingStartX, centerY), Vector2(Rect.maxX(), centerY),
                                            style.pixel(PixelIndex::Separator), thickness);
            }
            ui.ctx().renderText(Vector2(textX, Rect.y + (Rect.height - textSize.y) * 0.5f), Text,
                                style.pixel(PixelIndex::Text), ui.ctx().font(), 0.0f);
        }

      private:
        Ui &ui;
        u8StringView Text;
        bool IsBuilt = false;
        Rect Rect;
    };

    template <class... Args> inline auto Ui::text(u8StringView format, const Args &...args) noexcept -> TextBuilder {
        const auto str = detail::format(Ctx.arena(), format, args...);
        return TextBuilder(*this, str);
    }
    inline auto Ui::button(u8StringView text) noexcept -> ButtonBuilder { return ButtonBuilder(*this, text); }
    inline auto Ui::checkbox(u8StringView text) noexcept -> CheckboxBuilder { return CheckboxBuilder(*this, text); }
    inline auto Ui::radioButton(u8StringView text, int optionValue) noexcept -> RadioButtonBuilder {
        return RadioButtonBuilder(*this, text, optionValue);
    }
    template <typename T> inline auto Ui::slider(u8StringView text) noexcept -> SliderBuilder<T> {
        return SliderBuilder<T>(*this, text);
    }
    inline auto Ui::inputText(u8StringView text) noexcept -> TextInputBuilder { return TextInputBuilder(*this, text); }
    inline auto Ui::progressBar() noexcept -> ProgressBarBuilder { return ProgressBarBuilder(*this); }
    inline auto Ui::switchToggle(u8StringView text) noexcept -> SwitchToggleBuilder {
        return SwitchToggleBuilder(*this, text);
    }
    inline auto Ui::separator() noexcept -> SeparatorBuilder { return SeparatorBuilder(*this); }
    inline auto Ui::separatorText(u8StringView text) noexcept -> SeparatorTextBuilder {
        return SeparatorTextBuilder(*this, text);
    }
    inline auto Ui::keybind(u8StringView text) noexcept -> KeyBindBuilder {
        return KeyBindBuilder(*this, text, Ctx.persistentState().popup, Ctx.persistentState().keyBind);
    }
    // 一帧之内的一个窗口作用域。
    //
    // 窗口在第一次真正需要它的时候才开启（show()、begin() 或析构），而不是在构造时：
    // 这样链式设置的选项就能在 beginWindow() 之前全部写入 WindowParams，
    // 于是位置、尺寸、内边距和裁剪矩形从第一帧起就是最终值。
    //
    // 若在构造时开启，chrome 会先用默认矩形绘制一遍，之后每改一个选项都要靠
    // Context::applyWindowParams() 重建几何与裁剪矩形，而已经发出的 chrome
    // 只能等到下一帧才修正。
    //
    // 顺带的结果：只声明、从不使用（既不 show() 也不 begin()）的窗口不会再被创建，
    // 因此也不会留下一个被收缩到 windowMinimumSize 的空窗口。
    //
    // 生命周期要求：本对象必须在 ctx.endFrame() 之前析构，否则 endWindow() 会晚于帧的收尾
    // （登记表那时已经被重排用过）。把窗口放进内层作用域即可（示例就是这样写的）。
    struct WindowScope {
      public:
        explicit WindowScope(Context &ctx, u8StringView title) noexcept : Ctx(ctx) { Params.title = title; }

        ~WindowScope() noexcept { close(); }

        WindowScope(const WindowScope &) = delete;
        WindowScope &operator=(const WindowScope &) = delete;

        // 设置窗口的位置。默认情况下（PositionMode::FirstUseEver），
        // 只有在窗口首次声明时，设置的位置才会生效。因此，即使每帧
        // 都链式调用此方法，也不会阻止用户拖动窗口。
        // 对于必须固定在某个位置的 HUD 风格窗口，请使用 pin()。
        auto &&position(this auto &&self, Vector2 value) noexcept {
            self.Params.position = value;
            self.Params.hasPosition = true;
            return std::forward<decltype(self)>(self);
        }

        auto &&position(this auto &&self, float x, float y) noexcept {
            return std::forward<decltype(self)>(self).position(Vector2(x, y));
        }

        auto &&position(this auto &&self, Vector2 value, PositionMode mode) noexcept {
            self.Params.position = value;
            self.Params.hasPosition = true;
            self.Params.positionMode = mode;
            return std::forward<decltype(self)>(self);
        }

        // 每一帧都将窗口固定在指定的位置。
        auto &&pin(this auto &&self) noexcept {
            self.Params.positionMode = PositionMode::Always;
            return std::forward<decltype(self)>(self);
        }

        auto &&size(this auto &&self, Vector2 value) noexcept {
            self.Params.size = value;
            self.Params.hasWidth = true;
            self.Params.hasHeight = true;
            return std::forward<decltype(self)>(self);
        }

        auto &&size(this auto &&self, float width, float height) noexcept {
            return std::forward<decltype(self)>(self).size(Vector2(width, height));
        }

        WindowScope &padding(Vector2 value) noexcept {
            Params.padding = value;
            return *this;
        }

        WindowScope &titleBar(bool enabled = true) noexcept {
            Params.hasTitleBar = enabled;
            return *this;
        }

        WindowScope &movable(bool enabled = true) noexcept {
            Params.movable = enabled;
            return *this;
        }

        WindowScope &background(bool enabled = true) noexcept {
            Params.drawBackground = enabled;
            return *this;
        }

        WindowScope &collapsed(bool value) noexcept {
            Params.hasCollapsed = true;
            Params.collapsed = value;
            return *this;
        }

        WindowScope &open(const bool *openState) noexcept {
            Params.openState = openState;
            return *this;
        }

        template <class F>
            requires std::invocable<F &, Ui &>
        void show(F &&closure) {
            if (begin()) {
                Ui ui(Ctx);
                std::invoke(std::forward<F>(closure), ui);
            }
        }

        // 当窗口处于打开状态时返回 true。
        // 调用此方法会打开窗口：即使窗口仅被声明，随后只是检查其状态，
        // 它在当前帧中仍然会存在。
        bool begin() noexcept {
            openWindow();
            return Visible;
        }
        // 如果窗口处于打开状态，则结束该窗口。
        // 可以安全地重复调用此方法，也可以对从未打开过窗口的作用域调用此方法——
        // 在这种情况下，没有需要弹出的内容。
        void close() noexcept {
            if (Visible) {
                Ctx.endWindow();
                Visible = false;
            }
        }

      private:
        // 当首次有操作需要访问窗口时，才真正打开窗口。
        // 这样，在此之前通过链式调用设置的所有选项都已经保存到 Params 中，
        // beginWindow() 可以一次性计算出最终的窗口几何信息。
        // 此后的每次调用都只需要检查一个标志位。
        void openWindow() noexcept {
            if (Opened) {
                return;
            }
            Opened = true;
            Visible = Ctx.beginWindow(Params);
        }

        Context &Ctx;
        WindowParams Params;
        bool Opened = false;
        bool Visible = false;
    };
} // namespace ReZeroGui
