#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <ReZeroGui/Input.h>
#include <ReZeroGui/Context.h>

namespace ReZeroGui {
    namespace Platform {
        inline auto keyCodeFromVirtualKey(WPARAM virtualKey) noexcept -> Input::KeyCode {
            switch (virtualKey) {
            case VK_TAB:
                return Input::KeyCode::Tab;
            case VK_LEFT:
                return Input::KeyCode::LeftArrow;
            case VK_RIGHT:
                return Input::KeyCode::RightArrow;
            case VK_UP:
                return Input::KeyCode::UpArrow;
            case VK_DOWN:
                return Input::KeyCode::DownArrow;
            case VK_PRIOR:
                return Input::KeyCode::PageUp;
            case VK_NEXT:
                return Input::KeyCode::PageDown;
            case VK_HOME:
                return Input::KeyCode::Home;
            case VK_END:
                return Input::KeyCode::End;
            case VK_INSERT:
                return Input::KeyCode::Insert;
            case VK_DELETE:
                return Input::KeyCode::Delete;
            case VK_BACK:
                return Input::KeyCode::Backspace;
            case VK_SPACE:
                return Input::KeyCode::Space;
            case VK_RETURN:
                return Input::KeyCode::Enter;
            case VK_ESCAPE:
                return Input::KeyCode::Escape;
            case VK_OEM_1:
                return Input::KeyCode::Semicolon;
            case VK_OEM_PLUS:
                return Input::KeyCode::Equal;
            case VK_OEM_COMMA:
                return Input::KeyCode::Comma;
            case VK_OEM_MINUS:
                return Input::KeyCode::Minus;
            case VK_OEM_PERIOD:
                return Input::KeyCode::Period;
            case VK_OEM_2:
                return Input::KeyCode::Slash;
            case VK_OEM_3:
                return Input::KeyCode::GraveAccent;
            case VK_OEM_4:
                return Input::KeyCode::LeftBracket;
            case VK_OEM_5:
                return Input::KeyCode::Backslash;
            case VK_OEM_6:
                return Input::KeyCode::RightBracket;
            case VK_OEM_7:
                return Input::KeyCode::Apostrophe;
            case VK_CAPITAL:
                return Input::KeyCode::CapsLock;
            case VK_SCROLL:
                return Input::KeyCode::ScrollLock;
            case VK_NUMLOCK:
                return Input::KeyCode::NumLock;
            case VK_SNAPSHOT:
                return Input::KeyCode::PrintScreen;
            case VK_PAUSE:
                return Input::KeyCode::Pause;
            case VK_SHIFT:
                return Input::KeyCode::LeftShift;
            case VK_CONTROL:
                return Input::KeyCode::LeftControl;
            case VK_MENU:
                return Input::KeyCode::LeftAlt;
            case VK_LWIN:
                return Input::KeyCode::LeftSuper;
            case VK_RWIN:
                return Input::KeyCode::RightSuper;
            case VK_APPS:
                return Input::KeyCode::Menu;
            default:
                break;
            }

            if (virtualKey >= 'A' && virtualKey <= 'Z') {
                return static_cast<Input::KeyCode>(static_cast<std::uint8_t>(Input::KeyCode::A) + (virtualKey - 'A'));
            }
            if (virtualKey >= '0' && virtualKey <= '9') {
                return static_cast<Input::KeyCode>(static_cast<std::uint8_t>(Input::KeyCode::Digit0) +
                                                   (virtualKey - '0'));
            }
            if (virtualKey >= VK_F1 && virtualKey <= VK_F12) {
                return static_cast<Input::KeyCode>(static_cast<std::uint8_t>(Input::KeyCode::F1) +
                                                   (virtualKey - VK_F1));
            }
            if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9) {
                return static_cast<Input::KeyCode>(static_cast<std::uint8_t>(Input::KeyCode::Keypad0) +
                                                   (virtualKey - VK_NUMPAD0));
            }

            switch (virtualKey) {
            case VK_MULTIPLY:
                return Input::KeyCode::KeypadMultiply;
            case VK_ADD:
                return Input::KeyCode::KeypadAdd;
            case VK_SEPARATOR:
                return Input::KeyCode::KeypadEnter;
            case VK_SUBTRACT:
                return Input::KeyCode::KeypadSubtract;
            case VK_DECIMAL:
                return Input::KeyCode::KeypadDecimal;
            case VK_DIVIDE:
                return Input::KeyCode::KeypadDivide;
            default:
                return Input::KeyCode::None;
            }
        }

        /// Distinguishes left / right modifiers using the extended key bit and the
        /// key state, matching the usual Win32 idiom.
        inline auto disambiguateModifier(WPARAM virtualKey, LPARAM lParam) noexcept -> Input::KeyCode {
            const bool extended = (lParam & 0x01000000) != 0;
            switch (virtualKey) {
            case VK_SHIFT:
                return (::GetKeyState(VK_LSHIFT) & 0x8000) != 0 ? Input::KeyCode::LeftShift
                                                                : Input::KeyCode::RightShift;
            case VK_CONTROL:
                return extended ? Input::KeyCode::RightControl : Input::KeyCode::LeftControl;
            case VK_MENU:
                return extended ? Input::KeyCode::RightAlt : Input::KeyCode::LeftAlt;
            default:
                return keyCodeFromVirtualKey(virtualKey);
            }
        }

        //===----------------------------------------------------------------------===//
        // Message translation
        //===----------------------------------------------------------------------===//

        /// Feeds one window message into \p context.
        ///
        /// Returns true when the UI consumed the message, i.e. the host should not
        /// forward it to its own input handling (mouse and keyboard only).
        inline auto handleMessage(Context &context, UINT message, WPARAM wParam, LPARAM lParam) noexcept -> bool {
            Input &input = context.io();

            switch (message) {
            case WM_MOUSEMOVE:
                input.setMousePosition(
                    Vector2(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))));
                input.setMouseFocus(true);
                return false;

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                input.setMouseButtonDown(Input::MouseButton::Left, true);
                return context.wantsMouseCapture();

            case WM_LBUTTONUP:
                input.setMouseButtonDown(Input::MouseButton::Left, false);
                return context.wantsMouseCapture();

            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
                input.setMouseButtonDown(Input::MouseButton::Right, true);
                return context.wantsMouseCapture();

            case WM_RBUTTONUP:
                input.setMouseButtonDown(Input::MouseButton::Right, false);
                return context.wantsMouseCapture();

            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
                input.setMouseButtonDown(Input::MouseButton::Middle, true);
                return context.wantsMouseCapture();

            case WM_MBUTTONUP:
                input.setMouseButtonDown(Input::MouseButton::Middle, false);
                return context.wantsMouseCapture();

            case WM_XBUTTONDOWN: {
                const Input::MouseButton button =
                    GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? Input::MouseButton::X1 : Input::MouseButton::X2;
                input.setMouseButtonDown(button, true);
                return context.wantsMouseCapture();
            }

            case WM_XBUTTONUP: {
                const Input::MouseButton button =
                    GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? Input::MouseButton::X1 : Input::MouseButton::X2;
                input.setMouseButtonDown(button, false);
                return context.wantsMouseCapture();
            }

            case WM_MOUSEWHEEL:
                input.setMouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / 120.0f);
                return context.wantsMouseCapture();

            case WM_MOUSEHWHEEL:
                return context.wantsMouseCapture();

            case WM_MOUSELEAVE:
                input.setMouseFocus(false);
                return false;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                input.setKeyState(disambiguateModifier(wParam, lParam), true);
                return context.wantsKeyboardCapture();

            case WM_KEYUP:
            case WM_SYSKEYUP:
                input.setKeyState(disambiguateModifier(wParam, lParam), false);
                return context.wantsKeyboardCapture();

            case WM_CHAR: {
                // UTF-16 arrives one code unit at a time; surrogate pairs are reassembled.
                thread_local WCHAR pendingHighSurrogate = 0;
                const auto unit = static_cast<WCHAR>(wParam);
                if (unit >= 0xD800 && unit <= 0xDBFF) {
                    pendingHighSurrogate = unit;
                    return context.wantsKeyboardCapture();
                }
                if (unit >= 0xDC00 && unit <= 0xDFFF && pendingHighSurrogate != 0) {
                    const char32_t codepoint = 0x10000u +
                                               ((static_cast<char32_t>(pendingHighSurrogate) - 0xD800u) << 10) +
                                               (static_cast<char32_t>(unit) - 0xDC00u);
                    pendingHighSurrogate = 0;
                    if (context.wantsKeyboardCapture()) {
                        input.addCharacter(codepoint);
                        return true;
                    }
                    return false;
                }
                pendingHighSurrogate = 0;
                if (wParam >= 0x20 && context.wantsKeyboardCapture()) {
                    input.addCharacter(static_cast<char32_t>(unit));
                    return true;
                }
                return false;
            }

            case WM_SETFOCUS:
                input.setWindowFocus(true);
                return false;

            case WM_KILLFOCUS:
                input.setWindowFocus(false);
                input.reset();
                return false;

            case WM_SIZE: {
                const std::uint32_t width = static_cast<std::uint32_t>(LOWORD(lParam));
                const std::uint32_t height = static_cast<std::uint32_t>(HIWORD(lParam));
                context.displaySize(Vector2(static_cast<float>(width), static_cast<float>(height)));
                return false;
            }

            default:
                return false;
            }
        }
    } // namespace Platform
} // namespace ReZeroGui
