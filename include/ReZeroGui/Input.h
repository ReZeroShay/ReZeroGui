#pragma once

#include <cstddef>
#include <ReZeroGui/ReZeroLib.h>

namespace ReZeroGui {
    struct Input {
        enum class MouseButton : std::uint8_t {
            Left = 0,
            Right = 1,
            Middle = 2,
            X1 = 3,
            X2 = 4,
            Count = 5,
        };
        static constexpr std::size_t MouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

        enum class KeyCode : std::uint8_t {
            None = 0,
            Space,
            Apostrophe,
            Comma,
            Minus,
            Period,
            Slash,
            Digit0,
            Digit1,
            Digit2,
            Digit3,
            Digit4,
            Digit5,
            Digit6,
            Digit7,
            Digit8,
            Digit9,
            Semicolon,
            Equal,
            A,
            B,
            C,
            D,
            E,
            F,
            G,
            H,
            I,
            J,
            K,
            L,
            M,
            N,
            O,
            P,
            Q,
            R,
            S,
            T,
            U,
            V,
            W,
            X,
            Y,
            Z,
            LeftBracket,
            Backslash,
            RightBracket,
            GraveAccent,
            Escape,
            Enter,
            Tab,
            Backspace,
            Insert,
            Delete,
            RightArrow,
            LeftArrow,
            DownArrow,
            UpArrow,
            PageUp,
            PageDown,
            Home,
            End,
            CapsLock,
            ScrollLock,
            NumLock,
            PrintScreen,
            Pause,
            F1,
            F2,
            F3,
            F4,
            F5,
            F6,
            F7,
            F8,
            F9,
            F10,
            F11,
            F12,
            Keypad0,
            Keypad1,
            Keypad2,
            Keypad3,
            Keypad4,
            Keypad5,
            Keypad6,
            Keypad7,
            Keypad8,
            Keypad9,
            KeypadDecimal,
            KeypadDivide,
            KeypadMultiply,
            KeypadSubtract,
            KeypadAdd,
            KeypadEnter,
            LeftShift,
            LeftControl,
            LeftAlt,
            LeftSuper,
            RightShift,
            RightControl,
            RightAlt,
            RightSuper,
            Menu,
            Count,
        };
        static constexpr std::size_t KeyCodeCount = static_cast<std::size_t>(KeyCode::Count);

        inline static constexpr Array<const char8_t *, KeyCodeCount> KeyCodeNames{
            u8"",
            u8"Space",
            u8"'",
            u8",",
            u8"-",
            u8".",
            u8"/",
            u8"0",
            u8"1",
            u8"2",
            u8"3",
            u8"4",
            u8"5",
            u8"6",
            u8"7",
            u8"8",
            u8"9",
            u8";",
            u8"=",
            u8"A",
            u8"B",
            u8"C",
            u8"D",
            u8"E",
            u8"F",
            u8"G",
            u8"H",
            u8"I",
            u8"J",
            u8"K",
            u8"L",
            u8"M",
            u8"N",
            u8"O",
            u8"P",
            u8"Q",
            u8"R",
            u8"S",
            u8"T",
            u8"U",
            u8"V",
            u8"W",
            u8"X",
            u8"Y",
            u8"Z",
            u8"[",
            u8"\\",
            u8"]",
            u8"`",
            u8"Escape",
            u8"Enter",
            u8"Tab",
            u8"Backspace",
            u8"Insert",
            u8"Delete",
            u8"Right",
            u8"Left",
            u8"Down",
            u8"Up",
            u8"Page Up",
            u8"Page Down",
            u8"Home",
            u8"End",
            u8"Caps Lock",
            u8"Scroll Lock",
            u8"Num Lock",
            u8"Print Screen",
            u8"Pause",
            u8"F1",
            u8"F2",
            u8"F3",
            u8"F4",
            u8"F5",
            u8"F6",
            u8"F7",
            u8"F8",
            u8"F9",
            u8"F10",
            u8"F11",
            u8"F12",
            u8"Keypad 0",
            u8"Keypad 1",
            u8"Keypad 2",
            u8"Keypad 3",
            u8"Keypad 4",
            u8"Keypad 5",
            u8"Keypad 6",
            u8"Keypad 7",
            u8"Keypad 8",
            u8"Keypad 9",
            u8"Keypad .",
            u8"Keypad /",
            u8"Keypad *",
            u8"Keypad -",
            u8"Keypad +",
            u8"Keypad Enter",
            u8"Left Shift",
            u8"Left Ctrl",
            u8"Left Alt",
            u8"Left Super",
            u8"Right Shift",
            u8"Right Ctrl",
            u8"Right Alt",
            u8"Right Super",
            u8"Menu",
        };

        [[nodiscard]] static auto keyCodeName(KeyCode key) noexcept -> u8StringView {
            const auto index = static_cast<std::size_t>(key);
            return index < KeyCodeNames.size() ? KeyCodeNames[index] : u8"";
        }

        void setMousePosition(Vector2 position) noexcept { MousePosition = position; }

        void setMouseButtonDown(MouseButton button, bool down) noexcept {
            const std::size_t index = static_cast<std::size_t>(button);
            if (down && !MouseDown[index]) {
                MouseClicked[index] = true;
                MouseDownDuration[index] = 0.0f;
            } else if (!down && MouseDown[index]) {
                MouseReleased[index] = true;
            }
            MouseDown[index] = down;
        }

        void setMouseWheel(float delta) noexcept { MouseWheel += delta; }

        void setKeyState(KeyCode key, bool down) noexcept {
            if (key == KeyCode::None || key == KeyCode::Count) {
                return;
            }
            const std::size_t index = static_cast<std::size_t>(key);
            if (down && !KeyDown[index]) {
                KeyPressed[index] = true;
            } else if (!down && KeyDown[index]) {
                KeyReleased[index] = true;
            }
            KeyDown[index] = down;
        }

        void addCharacter(char32_t codepoint) noexcept { Characters.push_back(codepoint); }

        void setWindowFocus(bool focused) noexcept { WindowFocused = focused; }

        void setMouseFocus(bool focused) noexcept { MouseFocused = focused; }

        Vector2 currentMousePosition() const noexcept { return MousePosition; }
        Vector2 currentMouseDelta() const noexcept { return MouseDelta; }
        float currentMouseWheel() const noexcept { return MouseWheel; }

        bool isMouseDown(MouseButton button) const noexcept { return MouseDown[static_cast<std::size_t>(button)]; }

        bool isMouseClicked(MouseButton button) const noexcept {
            return MouseClicked[static_cast<std::size_t>(button)];
        }

        bool isMouseReleased(MouseButton button) const noexcept {
            return MouseReleased[static_cast<std::size_t>(button)];
        }

        bool isMouseDragging(MouseButton button) const noexcept {
            return MouseDown[static_cast<std::size_t>(button)] &&
                   MouseDownDuration[static_cast<std::size_t>(button)] > 0.0f;
        }

        bool isKeyDown(KeyCode key) const noexcept { return KeyDown[static_cast<std::size_t>(key)]; }

        bool isKeyPressed(KeyCode key) const noexcept { return KeyPressed[static_cast<std::size_t>(key)]; }

        bool isKeyReleased(KeyCode key) const noexcept { return KeyReleased[static_cast<std::size_t>(key)]; }

        bool isShiftDown() const noexcept { return isKeyDown(KeyCode::LeftShift) || isKeyDown(KeyCode::RightShift); }

        bool isControlDown() const noexcept {
            return isKeyDown(KeyCode::LeftControl) || isKeyDown(KeyCode::RightControl);
        }

        bool isAltDown() const noexcept { return isKeyDown(KeyCode::LeftAlt) || isKeyDown(KeyCode::RightAlt); }

        Span<const char32_t> currentCharacters() const noexcept { return Characters.asSpan(); }

        bool isWindowFocused() const noexcept { return WindowFocused; }
        bool isMouseFocused() const noexcept { return MouseFocused; }

        // -- frame boundaries --------------------------------------------------

        void onFrameBegin(float deltaTime) noexcept {
            DeltaTime = deltaTime;
            // Positions arrive between frames; the delta is what the mouse moved since
            // the previous frame was presented.
            MouseDelta = Vector2(MousePosition.x - PreviousMousePosition.x, MousePosition.y - PreviousMousePosition.y);
        }

        void onFrameEnd() noexcept {
            for (std::size_t index = 0; index < MouseButtonCount; ++index) {
                if (MouseDown[index]) {
                    MouseDownDuration[index] += DeltaTime;
                }
                MouseClicked[index] = false;
                MouseReleased[index] = false;
            }
            KeyPressed.fill(false);
            KeyReleased.fill(false);
            Characters.clear();
            MouseWheel = 0.0f;
            PreviousMousePosition = MousePosition;
        }

        /// Drops all state, e.g. when the host window loses focus.
        void reset() noexcept {
            MouseDown.fill(false);
            MouseClicked.fill(false);
            MouseReleased.fill(false);
            MouseDownDuration.fill(0.0f);
            KeyDown.fill(false);
            KeyPressed.fill(false);
            KeyReleased.fill(false);
            Characters.clear();
            MouseWheel = 0.0f;
            MouseDelta = Vector2{};
        }

      private:
        Vector2 MousePosition;
        Vector2 PreviousMousePosition;
        Vector2 MouseDelta;
        float MouseWheel = 0.0f;
        float DeltaTime = 0.0f;

        Array<bool, MouseButtonCount> MouseDown{};
        Array<bool, MouseButtonCount> MouseClicked{};
        Array<bool, MouseButtonCount> MouseReleased{};
        Array<float, MouseButtonCount> MouseDownDuration{};

        Array<bool, KeyCodeCount> KeyDown{};
        Array<bool, KeyCodeCount> KeyPressed{};
        Array<bool, KeyCodeCount> KeyReleased{};

        InplaceVector<char32_t, 32> Characters;
        bool WindowFocused = true;
        bool MouseFocused = true;
    };
} // namespace ReZeroGui