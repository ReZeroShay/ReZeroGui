#include "ReZeroGui/Context.h"
#include <chrono>
#include <ReZeroGui/ReZeroGui.h>
inline static int clickCount = 0;
inline bool wireframe = true;
inline bool showGrid = false;
inline int shading = 1;
inline float exposure = 0.5f;
inline int samples = 8;
inline char8_t name[32] = u8"ReZero";
inline float progress = 0.35f;
inline ReZeroGui::Input::KeyCode duckKey = ReZeroGui::Input::KeyCode::None;

// "##" 示例用的宿主状态：两个按钮看起来一模一样，但各自计数。
inline int labelIdLeftClicks = 0;
inline int labelIdRightClicks = 0;
inline float labelIdVolume = 0.5f;
inline bool labelIdEnabled = false;

inline void drawDemo(ReZeroGui::Ui &ui) {
    ui.separatorText(u8"button");
    ui.text(u8"clicks: {}", clickCount);
    ui.button(u8"Click me").onClick([](auto &) { clickCount += 1; });
    ui.button(u8" ").fillWidth().onClick([](auto &) { clickCount += 1; });
    ui.button(u8"disabled").fillWidth().disabled();
    ui.separatorText(u8"checkbox");
    ui.checkbox(u8"Wireframe").checked(&wireframe);
    ui.checkbox(u8"Show grid").checked(&showGrid).alignment(ReZeroGui::HorizontalAlignment::End);
    ui.checkbox(u8"Show grid1").checked(&showGrid).alignment(ReZeroGui::HorizontalAlignment::SpaceBetween);
    ui.separatorText(u8"radiobutton");
    ui.radioButton(u8"Flat", 0).selected(&shading);
    ui.radioButton(u8"Smooth", 1).selected(&shading);
    char8_t counter[48];
    std::snprintf((char *)counter, sizeof(counter), "clicks: %d", clickCount);
    // ui.text(counter);
    ui.separator();
    ui.separator();
    ui.separator();
    ui.text(u8"Shading");

    ui.separator();

    ui.keybind(u8"Smooth1").key(&duckKey);

    ui.slider<float>(u8"Exposure").value(&exposure).range(0.0f, 2.0f);
    ui.slider<int>(u8"Samples").value(&samples).range(1, 16);
    ui.slider<int>(u8"Samples1").value(&samples).range(1, 16);
    ui.inputText(u8"Name").buffer(name).hint(u8"type a name");
    ui.progressBar().fraction(progress).overlay(u8"35%");
    ui.switchToggle(u8"toggle").checked(&wireframe);
    ui.switchToggle(u8"toggle1")
        .checked(&wireframe)
        .direction(ReZeroGui::LayoutDirection::Reverse)
        .alignment(ReZeroGui::HorizontalAlignment::SpaceBetween);
}

inline void drawDemo2(ReZeroGui::Ui &ui) {
    ui.text(u8"hello world");
    if (ui.button(u8"Click me").fillWidth().onClick([](auto &) { clickCount += 1; }).clicked()) {
        std::fprintf(stderr, "button clicked\n");
    }
    ui.separatorText(u8"cool");
    ui.checkbox(u8"Wireframe11").checked(&wireframe);
}

// 标签里 "##" 的用法示例。
//
// 关键点：显示文本和控件身份是两回事。"##" 之前进界面，"##" 之后只进 ID 哈希。
inline void drawLabelIdDemo(ReZeroGui::Ui &ui) {
    ui.separatorText(u8"two controls, one visible name");
    ui.text(u8"left {}   right {}", labelIdLeftClicks, labelIdRightClicks);

    // 两个按钮显示的都是 "Reset"，但后缀不同，所以是各自独立的控件：点左边只加左边。
    // 如果两个都写成 u8"Reset"，它们会算出同一个 ID，点一个两个都响应。
    ui.button(u8"Reset##left").onClick([](auto &) { labelIdLeftClicks += 1; });
    ui.sameLine();
    ui.button(u8"Reset##right").onClick([](auto &) { labelIdRightClicks += 1; });

    ui.separatorText(u8"the label may change, the identity may not");
    // 标签里带上当前值：拖动时文字每帧都在变，但 "##volume" 让 ID 保持不变。
    // 去掉 "##volume" 的话，拖动过程中控件会"变成另一个控件"，值会断在半路。
    char8_t volumeLabel[32];
    std::snprintf(reinterpret_cast<char *>(volumeLabel), sizeof(volumeLabel), "Volume %.2f##volume",
                  static_cast<double>(labelIdVolume));
    ui.slider<float>(volumeLabel).value(&labelIdVolume).range(0.0f, 1.0f);
    ui.text(u8"volume {}", labelIdVolume);

    ui.separatorText(u8"other labelled controls work the same way");
    ui.checkbox(u8"Enabled##enabled").checked(&labelIdEnabled);
    ui.switchToggle(u8"Power##power").checked(&labelIdEnabled);
    ui.inputText(u8"Name##name").buffer(name).hint(u8"type a name");

    ui.separatorText(u8"content, not labels: ## is literal here");
    ui.text(u8"text() draws content, so ## shows up as typed");
    ui.text(u8"and formatting keeps it too: {} ## visible", 1);
}
auto lastTime = std::chrono::steady_clock::now();
float fps = 0.;

inline void showFPS(ReZeroGui::Context &ctx, uint32_t &frameCount) {

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - lastTime;
    if (elapsed >= std::chrono::seconds(1)) {
        fps = static_cast<float>(frameCount) / std::chrono::duration<float>(elapsed).count();

        frameCount = 0;
        lastTime = now;
    }
    const auto displaySize = ctx.displaySize();
    ReZeroGui::WindowScope panel(ctx, (char8_t *)u8"FPS");
    panel.position(displaySize.x - 150.0f, 30.0f).titleBar(false).pin().show([](ReZeroGui::Ui &ui) {
        ui.text(u8"FPS: {}", fps);
    });
}

inline void showExample(ReZeroGui::Context &ctx) {
    {
        ReZeroGui::WindowScope panel(ctx, (char8_t *)u8"ReZeroGui");
        panel.position(40.0f, 40.0f).show(&drawDemo);
    }
    {
        ctx.style().light();
        ctx.style().frameRounding = 0;
        ctx.style().frameBorderSize = 0;
        ctx.style().windowBorderSize = 0;

        // ctx.style().pixel(PixelIndex index, Pixel value);
        ReZeroGui::WindowScope panel(ctx, (char8_t *)u8"Demo");
        panel.position(400.0f, 400.0f).show(drawDemo2);
    }
    {
        // 窗口标题也走 "##" 约定：标题栏显示的是 "Labels"，身份是 "labels"。
        // 把显示文本换成别的语言，窗口的位置、尺寸、折叠状态都还在。
        ReZeroGui::WindowScope panel(ctx, (char8_t *)u8"Labels##labels");
        panel.position(760.0f, 40.0f).show(drawLabelIdDemo);
    }
    static uint32_t frameCount = 0;
    frameCount += 1;
    showFPS(ctx, frameCount);
}
