#pragma once
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Id.h>

// 持久化状态
// 所有控件需要从当前帧保留到下一帧的数据，都存储在这里。
// 即时模式（Immediate Mode）会在每一帧重新构建整个 UI。控件只是临时创建的，
// 因此无法依靠自身来保存数据。
// 有些数据必须在不同帧之间持续存在，例如动画状态、键盘焦点、展开状态等。
namespace ReZeroGui {

    // 按控件 ID 索引的动画值。
    //
    // 每个槽位都会记录自己最后一次被访问时的帧编号，并在 endFrame() 中清理。
    // 因此，如果某个控件不再被绘制，它遗留的动画状态就不会一直保留下去；
    // 如果该控件之后重新出现，它的动画值会直接从目标值开始，而不会重新播放动画。
    //
    // 这里使用固定大小的数组，是因为动画状态必须比帧临时内存区域（Frame Arena）存活得更久，
    // 同时，稳定的存储地址也使得读取槽位时无需进行内存分配。
    class AnimationState {
        struct AnimationSlot {
            Id id = InvalidId;
            std::uint32_t lastFrame = 0;
            float value = 0.0f;
        };

        static constexpr std::size_t Capacity = 128;

      public:
        // 将 id 对应的值向 target 推进一帧，并返回更新后的值。
        // 对于新 ID，直接将其值初始化为 target。
        // 因此，首次出现的控件不会播放补帧动画；
        // 如果控件曾消失一帧，它也会重新开始，而不是接着之前的动画继续播放。
        //
        // 不会通过引用返回槽位中的值：控件一旦不再被声明，其槽位就会被回收，
        // 因此，如果保存了该槽位的引用，之后就可能在不知情的情况下访问到其他控件的动画状态。
        auto advance(Id id, float target, float tau, float dt, std::uint32_t frame) noexcept -> float {
            std::size_t freeIndex = Capacity;
            for (std::size_t index = 0; index < Capacity; index += 1) {
                auto &slot = Slots[index];
                if (slot.id == id) {

                    REZERO_ASSERT_MSG(slot.lastFrame != frame, "two widgets share one id this frame");
                    slot.lastFrame = frame;
                    slot.value = damp(slot.value, target, tau, dt);
                    return slot.value;
                }
                if (freeIndex == Capacity && slot.id == InvalidId) {
                    freeIndex = index;
                }
            }
            if (freeIndex < Capacity) {
                AnimationSlot &slot = Slots[freeIndex];
                slot.id = id;
                slot.lastFrame = frame;
                slot.value = target;
                return slot.value;
            }
            REZERO_ASSERT_MSG(false, "animation store is full");
            return target;
        }
        void sweep(std::uint32_t frame) noexcept {
            for (AnimationSlot &slot : Slots) {
                if (slot.id != InvalidId && slot.lastFrame != frame) {
                    slot.id = InvalidId;
                }
            }
        }

      private:
        Array<AnimationSlot, Capacity> Slots;
    };

    // 当前拥有键盘输入焦点的文本框，以及该文本框中的光标位置。
    //
    // 这里只需要一个槽位，而不是一张状态表：同一时间只能编辑一个文本框。
    // 这也是光标位置保存在这里，而不是作为控件的输入输出参数传递的原因。
    //
    // focused 有意与 InteractionStore::active 区分开来：
    // 文本编辑状态会一直持续到按下 Enter、Escape 或点击其他位置为止；
    // 而鼠标操作的所有权只会持续到一次按压操作结束。
    struct TextEditState {
        Id focused = InvalidId;

        // 当前获得焦点的文本框最后一次被声明时所在的帧。
        // endFrame() 会利用它清除已经消失的文本框的焦点。
        std::uint32_t frame = 0;

        // 光标在文本框缓冲区中的字节偏移量，始终位于一个 Unicode 码点的边界上。
        std::size_t caret = 0;
    };

    // 当前打开的弹出窗口，无论它由哪个控件打开，以及该弹出窗口占据的矩形区域。
    //
    // 该状态由 Context 保存，因为打开的弹出窗口会对鼠标操作形成模态限制：
    // 每个控件在检测鼠标悬停时，都必须知道用于关闭弹出窗口的形状。
    // 而打开弹出窗口的控件本身是临时创建的，无法承担这份持久化状态。
    struct PopupState {
        Id id = InvalidId;
        Rect bounds;

        // 弹出窗口最后一次被声明时所在的帧。
        // endFrame() 会关闭其对应控件已经消失的弹出窗口。
        std::uint32_t frame = 0;
    };

    // 正在等待用户按键输入的按键绑定项。
    struct KeyBindState {
        Id listening = InvalidId;
        // 正在监听按键的控件最后一次被声明时所在的帧。
        // 如果该控件消失，就不能让它继续占用键盘输入。
        std::uint32_t frame = 0;
    };

    // 当前拥有鼠标操作权的对象。
    //
    // active 会有意地跨帧保留：它表示当前正被按住的按钮，或者正在被拖动的窗口。
    // 因此，即使每一帧都会重新读取输入状态，它也不能仅仅作为当前帧的临时状态存在。
    //
    // 但"跨帧"不等于"无限期"：frame 记录属主最后一次确认这次鼠标归属是哪一帧。如果属主某一帧
    // 没被声明（控件消失、窗口没画），它就没机会在用户松开鼠标时清掉 active，而
    // isMouseHoveringRect() 会一直拒绝其它所有 id——整个界面从此再也点不动。endFrame() 因此和
    // textEdit/popup 一样按帧号扫掉没人认领的 active。
    //
    // 该状态保存在 PersistentState 中，而不是直接放在 Context 上，
    // 这样控件引擎就可以通过 persistentState() 访问它，而无需直接访问私有状态。
    struct InteractionState {
        Id active = InvalidId;
        // 属主最后一次确认这次鼠标归属的帧号；见 InteractionState 的说明。
        std::uint32_t frame = 0;
    };

    /// Everything that has to outlive a frame, grouped by what owns it.
    struct PersistentState {
        AnimationState animations;
        TextEditState textEdit;
        PopupState popup;
        KeyBindState keyBind;
        InteractionState interaction;

        // 清除所有在 frameIndex 对应帧中未被访问的状态，
        // 防止控件停止被声明后，仍然遗留过期的焦点、弹出窗口、按键绑定或鼠标归属。
        //
        // animations.sweep() 则是针对动画状态表实现的同一套清理规则。
        // 所有五类状态都在这里统一清理，而不是将清理逻辑分散到帧结束流程的各个位置。
        void endFrame(std::uint32_t frameIndex) noexcept {
            if (textEdit.focused != InvalidId && textEdit.frame != frameIndex) {
                textEdit.focused = InvalidId;
            }
            if (keyBind.listening != InvalidId && keyBind.frame != frameIndex) {
                keyBind.listening = InvalidId;
            }
            if (popup.id != InvalidId && popup.frame != frameIndex) {
                popup.id = InvalidId;
                popup.bounds = Rect();
            }
            if (interaction.active != InvalidId && interaction.frame != frameIndex) {
                interaction.active = InvalidId;
            }
            animations.sweep(frameIndex);
        }
    };

} // namespace ReZeroGui