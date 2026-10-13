#pragma once

#include <cstdint>
#include <cstddef>
#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {
    enum class PixelIndex : std::uint8_t {
        Text,
        TextDisabled,
        WindowBackground,
        WindowBorder,
        TitleBackground,
        TitleBackgroundActive,
        TitleBackgroundCollapsed,
        Border,
        BorderShadow,
        FrameBackground,
        FrameBackgroundHovered,
        FrameBackgroundActive,
        Button,
        ButtonHovered,
        ButtonActive,
        CheckMark,
        SliderFill,
        SliderGrab,
        SliderGrabActive,
        Separator,
        TextSelectionBackground,
        ProgressBar,
        SwitchToggleBlock,
        Count,
    };

    constexpr std::size_t ColorIndexCount = static_cast<std::size_t>(PixelIndex::Count);

    struct Style {
        // Window
        Vector2 windowPadding{10.0f, 10.0f};
        Vector2 windowMinimumSize{0.0f, 0.0f};
        float windowRounding = 6.0f;
        float windowBorderSize = 1.0f;
        float windowTitleAlignX = 0.0f;

        // Frame (buttons, sliders, inputs)
        Vector2 framePadding{8.0f, 5.0f};
        float frameRounding = 4.0f;
        float frameBorderSize = 1.0f;

        float thinTrackThickness = 4.0f;

        // Layout
        Vector2 itemSpacing{8.0f, 6.0f};
        Vector2 itemInnerSpacing{6.0f, 4.0f};
        float indentSpacing = 20.0f;
        float grabMinimumSize = 12.0f;

        // Text
        float fontSize = 20.0f;
        Vector2 buttonTextAlign{0.5f, 0.5f};

        // Misc
        float dpiScale = 1.0f;

        Pixel colors[ColorIndexCount];

        constexpr Style() noexcept { resetToDefault(); }

        constexpr void resetToDefault() noexcept {
            colors[static_cast<std::size_t>(PixelIndex::Text)] = Pixel::fromRgba(0xFFFFFFFF);
            colors[static_cast<std::size_t>(PixelIndex::TextDisabled)] = Pixel::fromRgba(0x9A9A9AFF);
            colors[static_cast<std::size_t>(PixelIndex::WindowBackground)] = Pixel::fromRgba(0x14161AEE);
            colors[static_cast<std::size_t>(PixelIndex::WindowBorder)] = Pixel::fromRgba(0x2C313AFF);
            colors[static_cast<std::size_t>(PixelIndex::TitleBackground)] = Pixel::fromRgba(0x1C2029FF);
            colors[static_cast<std::size_t>(PixelIndex::TitleBackgroundActive)] = Pixel::fromRgba(0x2A4C78FF);
            colors[static_cast<std::size_t>(PixelIndex::TitleBackgroundCollapsed)] = Pixel::fromRgba(0x181B21FF);
            colors[static_cast<std::size_t>(PixelIndex::Border)] = Pixel::fromRgba(0x3A414DFF);
            colors[static_cast<std::size_t>(PixelIndex::BorderShadow)] = Pixel::fromRgba(0x00000055);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackground)] = Pixel::fromRgba(0x22262EFF);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackgroundHovered)] = Pixel::fromRgba(0x2B303AFF);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackgroundActive)] = Pixel::fromRgba(0x36404FFF);
            colors[static_cast<std::size_t>(PixelIndex::Button)] = Pixel::fromRgba(0x2F6FB2FF);
            colors[static_cast<std::size_t>(PixelIndex::ButtonHovered)] = Pixel::fromRgba(0x3A83CFFF);
            colors[static_cast<std::size_t>(PixelIndex::ButtonActive)] = Pixel::fromRgba(0x245FA0FF);
            colors[static_cast<std::size_t>(PixelIndex::CheckMark)] = Pixel::fromRgba(0x6FC3FFFF);
            colors[static_cast<std::size_t>(PixelIndex::SliderGrab)] = Pixel::fromRgba(0x8A93A6FF);
            colors[static_cast<std::size_t>(PixelIndex::SliderGrabActive)] = Pixel::fromRgba(0xC7D1E5FF);
            // 这两个原来漏了赋值，于是保持 Pixel() 的默认值 (0,0,0,1)：开关的滑块会是不透明的纯黑，
            // 而 light() 里恰好设了 SwitchToggleBlock，问题只在默认皮肤下出现。
            colors[static_cast<std::size_t>(PixelIndex::SliderFill)] = Pixel::fromRgba(0x2F6FB2FF);
            colors[static_cast<std::size_t>(PixelIndex::SwitchToggleBlock)] = Pixel::fromRgba(0xE8ECF5FF);
            colors[static_cast<std::size_t>(PixelIndex::Separator)] = Pixel::fromRgba(0x3A414DFF);
            colors[static_cast<std::size_t>(PixelIndex::TextSelectionBackground)] = Pixel::fromRgba(0x2F6FB299);
            colors[static_cast<std::size_t>(PixelIndex::ProgressBar)] = Pixel::fromRgba(0x2F6FB2FF);
        }

        constexpr void light() noexcept {

            colors[static_cast<std::size_t>(PixelIndex::TitleBackground)] = Pixel(0.96f, 0.96f, 0.96f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::TitleBackgroundActive)] = Pixel(0.82f, 0.82f, 0.82f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::TitleBackgroundCollapsed)] = Pixel(1.00f, 1.00f, 1.00f, 0.51f);

            colors[static_cast<std::size_t>(PixelIndex::Text)] = Pixel(0.00f, 0.00f, 0.00f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::TextDisabled)] = Pixel(0.60f, 0.60f, 0.60f, 1.00f);

            colors[static_cast<std::size_t>(PixelIndex::WindowBackground)] = Pixel(0.94f, 0.94f, 0.94f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::WindowBorder)] = Pixel(0.00f, 0.00f, 0.00f, 0.30f);
            colors[static_cast<std::size_t>(PixelIndex::SliderFill)] = Pixel::fromRgba(0x2F6FB2FF);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackground)] = Pixel(1.00f, 1.00f, 1.00f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackgroundHovered)] = Pixel(0.26f, 0.59f, 0.98f, 0.40f);
            colors[static_cast<std::size_t>(PixelIndex::FrameBackgroundActive)] = Pixel(0.26f, 0.59f, 0.98f, 0.67f);
            colors[static_cast<std::size_t>(PixelIndex::Button)] = Pixel(0.26f, 0.59f, 0.98f, 0.40f);
            colors[static_cast<std::size_t>(PixelIndex::ButtonHovered)] = Pixel(0.26f, 0.59f, 0.98f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::ButtonActive)] = Pixel(0.06f, 0.53f, 0.98f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::SwitchToggleBlock)] = Pixel(0.26f, 0.59f, 0.98f, 1.00f);
            colors[static_cast<std::size_t>(PixelIndex::CheckMark)] = Pixel(0.26f, 0.59f, 0.98f, 1.00f);

            colors[static_cast<std::size_t>(PixelIndex::SliderGrab)] = Pixel(0.26f, 0.59f, 0.98f, 0.78f);
            colors[static_cast<std::size_t>(PixelIndex::SliderGrabActive)] = Pixel(0.46f, 0.54f, 0.80f, 0.60f);

            colors[static_cast<std::size_t>(PixelIndex::ProgressBar)] = Pixel(0.26f, 0.59f, 0.98f, 0.78f);
        }

        constexpr Pixel pixel(PixelIndex index) const noexcept { return colors[static_cast<std::size_t>(index)]; }

        constexpr Style &pixel(PixelIndex index, Pixel value) noexcept {
            colors[static_cast<std::size_t>(index)] = value;
            return *this;
        }
        constexpr auto scaled(float scale) const noexcept -> Style {

            Style result = *this;
            result.windowPadding = windowPadding * scale;
            result.windowMinimumSize = windowMinimumSize * scale;
            result.windowRounding = windowRounding * scale;
            result.windowBorderSize = windowBorderSize * scale;
            result.framePadding = framePadding * scale;
            result.frameRounding = frameRounding * scale;
            result.frameBorderSize = frameBorderSize * scale;
            result.thinTrackThickness = thinTrackThickness * scale;
            result.itemSpacing = itemSpacing * scale;
            result.itemInnerSpacing = itemInnerSpacing * scale;
            result.indentSpacing = indentSpacing * scale;
            result.grabMinimumSize = grabMinimumSize * scale;
            result.fontSize = fontSize * scale;
            result.dpiScale = scale;
            return result;
        }
    };
} // namespace ReZeroGui
