#pragma once
#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {
    // 控件的唯一标识符。
    //
    // 在即时模式 GUI（Immediate Mode GUI，IMGUI）中，同一个控件通常会在每一帧
    // 重新创建或重新提交，因此不能依赖控件对象本身来保存其状态。
    // 需要使用稳定的 ID 来识别同一个控件，从而在不同帧之间关联和保留控件状态，
    // 例如按钮状态、复选框选中状态、输入框编辑状态等。
    //
    // 控件 ID 可以通过哈希控件标签（Label）和当前作用域的 ID（Scope ID）生成。
    // 将作用域 ID 纳入计算，可以区分不同窗口、容器或作用域中的同名控件。
    //
    // 使用 uint32_t 存储 ID，意味着 ID 的取值范围为 0 到 UINT32_MAX。
    using Id = std::uint32_t;
    // 无效控件 ID。
    constexpr Id InvalidId = std::numeric_limits<Id>::max();

    // ---- "##" 约定 --------------------------------------------------------------
    //
    // 控件标签里可以写一次 "##"，把它分成两半：前一半是显示给用户看的文本，后一半是控件
    // 身份。取身份的只有后一半，于是"改显示文本"就不再等于"换一个控件"：
    //
    //   ui.slider<float>(u8"Volume##volume").value(&volume);   // 标题栏/标签显示 "Volume"
    //   ui.slider<float>(u8"音量##volume").value(&volume);     // 还是同一个滑块，拖动状态不丢
    //
    // 反过来它也让同名控件能共存——同一个窗口里两个都叫 "Reset" 的按钮，只写 "Reset" 会撞 ID
    //（一个被点击，两个都响应），加上后缀就分开了：
    //
    //   ui.button(u8"Reset##left")  /  ui.button(u8"Reset##right")
    //
    // 没有 "##" 时整串既是显示文本也是身份，也就是老行为，所以这个约定是可选的。
    //
    // 两个函数都返回原串的非拥有视图，不复制也不分配。走这个约定的控件有：button、checkbox、
    // radioButton、switchToggle、slider、inputText、keybind，以及窗口标题（WindowScope）。
    // text()/separatorText()/progressBar().overlay() 不在其中：它们画的是内容而不是
    // 标签，没有身份可言，所以 "##" 在那里就是字面文本。

    // 标签的显示文本："##" 之前的部分；没有 "##" 时就是整串。
    inline auto labelText(u8StringView label) noexcept -> u8StringView {
        const auto marker = label.find(u8"##");
        return marker == u8StringView::npos ? label : label.substr(0, marker);
    }

    // 标签的身份部分："##" 之后的部分；没有 "##" 时就是整串。
    //
    // 以 "##" 结尾（身份为空）是用法错误：那样所有这样写的控件都会拿到同一个空身份，从而互相
    // 撞 ID。与其静默地让两个控件共享状态，不如断言出来，并退回整串当作身份（至少彼此不同）。
    inline auto labelId(u8StringView label) noexcept -> u8StringView {
        const auto marker = label.find(u8"##");
        if (marker == u8StringView::npos) {
            return label;
        }
        const auto id = label.substr(marker + 2);
        if (id.empty()) {
            REZERO_ASSERT_MSG(false, "labelId: a label must not end with \"##\" (empty identity)");
            return label;
        }
        return id;
    }
} // namespace ReZeroGui
