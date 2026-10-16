#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <cstdint>

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
                // 最小化也会送来一条 WM_SIZE，但客户区是空的。照着它更新的话，窗口被收起来的
                // 那段时间里所有窗口的布局都会被压成 0x0 的显示尺寸，所以只发布真实的尺寸；
                // 反正 Platform::Win32 每帧还会重新读一次客户区。
                if (width == 0 || height == 0) {
                    return false;
                }
                context.displaySize(Vector2(static_cast<float>(width), static_cast<float>(height)));
                return false;
            }

            default:
                return false;
            }
        }

        //===----------------------------------------------------------------------===//
        // 帧的生命周期
        //===----------------------------------------------------------------------===//

        /// 一个 Context 加一个 HWND 的逐帧杂务：Context::beginFrame(deltaTime) 要用的计时器，
        /// 以及布局所依据的客户区尺寸。
        ///
        ///     ReZeroGui::Platform::Win32 platform;
        ///     if (!platform.initialize(ctx, window)) {
        ///         return 1;
        ///     }
        ///     // 窗口过程照旧把每条消息转发给 ctx，见 handleMessage()。
        ///     uiContext = &ctx;
        ///     for (;;) {
        ///         // 消息循环归宿主：循环什么时候结束、队列还用来做什么，只有宿主知道。
        ///         MSG message{};
        ///         while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ///             if (message.message == WM_QUIT) {
        ///                 return 0;
        ///             }
        ///             ::TranslateMessage(&message);
        ///             ::DispatchMessageW(&message);
        ///         }
        ///
        ///         ctx.beginFrame(platform.newFrame(ctx));
        ///         // 在这里声明窗口和控件
        ///         ctx.endFrame();
        ///         renderer.renderDrawData(ctx.drawItems());
        ///         // present
        ///     }
        ///
        /// newFrame() 只采样计时器、刷新显示尺寸；它刻意不抽消息队列、也不开帧，因为这两件事
        /// 都该由宿主决定。
        struct Win32 {
            Win32() noexcept = default;
            Win32(const Win32 &) = delete;
            Win32 &operator=(const Win32 &) = delete;

            /// 把 \p window 接到 \p context 上并启动计时器。进入循环之前调用一次即可。
            ///
            /// 没有窗口、或者拿不到可用的高精度计时器时返回 false，此时宿主不应进入帧循环。
            [[nodiscard]] auto initialize(Context &context, HWND window) noexcept -> bool {
                if (window == nullptr) {
                    REZERO_ASSERT_MSG(false, "Platform::Win32::initialize() needs a window");
                    return false;
                }
                if (Window != nullptr) {
                    REZERO_ASSERT_MSG(false, "Platform::Win32::initialize() was already called");
                    return false;
                }

                LARGE_INTEGER frequency{};
                if (::QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart == 0) {
                    // 本库支持的 Windows 都有性能计数器，走到这里说明机器本身已经出问题了。
                    // 直接拒绝，好过让宿主跑一个 delta time 恒为 0 的循环。
                    REZERO_ASSERT_MSG(false, "Platform::Win32::initialize(): no high resolution timer");
                    return false;
                }

                Window = window;
                Frequency = frequency.QuadPart;
                PreviousCounter = readCounter();
                // 首帧从这里量起，而不是从进程启动算起。
                DeltaTime = 0.0f;

                // 窗口这时已经建好了，客户区就是首帧的显示尺寸。
                updateDisplaySize(context);
                return true;
            }

            /// 忘掉窗口、清掉计时器状态。宿主在循环结束之后、Context 与窗口销毁之前调用。
            void shutdown() noexcept {
                Window = nullptr;
                Frequency = 0;
                PreviousCounter = 0;
                DeltaTime = 0.0f;
            }

            /// 每帧调用一次，位置在宿主抽完自己的消息队列之后：采样计时器并刷新显示尺寸。
            ///
            /// 返回距上一次调用经过的秒数，也就是 Context::beginFrame() 要的那个值；事后
            /// deltaTime() 读到的也是同一个值。
            auto newFrame(Context &context) noexcept -> float {
                if (Window == nullptr) {
                    REZERO_ASSERT_MSG(false, "Platform::Win32::newFrame() before initialize()");
                    return 0.0f;
                }

                const std::int64_t counter = readCounter();
                // 两帧落在同一个计时器滴答里时 delta 正好是 0，界面读到的是"这一帧没花时间"：
                // 这是对的，也是帧率再高也快不过计时器分辨率的原因。
                DeltaTime = Frequency > 0 ? static_cast<float>(static_cast<double>(counter - PreviousCounter) /
                                                               static_cast<double>(Frequency))
                                          : 0.0f;
                PreviousCounter = counter;

                updateDisplaySize(context);
                // Win32 的"鼠标离开"每次登记只报一次，所以要为下一次重新登记。每帧登记一遍是
                // 幂等的，也正是它让 Input::isMouseFocused() 一直说的是真话。
                trackMouseLeave();
                return DeltaTime;
            }

            /// 读窗口的客户区，并作为 Context 的显示尺寸发布出去。
            ///
            /// 没东西可发布时返回 false——窗口已经没了，或者它最小化了、客户区是空的。后一种
            /// 情况保留上一次的尺寸，正是它挡住了"最小化的窗口把布局压成 0x0"。
            auto updateDisplaySize(Context &context) noexcept -> bool {
                RECT client{};
                if (Window == nullptr || ::GetClientRect(Window, &client) == FALSE) {
                    return false;
                }
                const float width = static_cast<float>(client.right - client.left);
                const float height = static_cast<float>(client.bottom - client.top);
                if (width <= 0.0f || height <= 0.0f) {
                    return false;
                }
                DisplaySize = Vector2(width, height);
                context.displaySize(DisplaySize);
                return true;
            }

            [[nodiscard]] auto window() const noexcept -> HWND { return Window; }

            /// 最近两次 newFrame() 之间经过的秒数，也就是那次调用的返回值；交给
            /// Context::beginFrame() 用。
            [[nodiscard]] auto deltaTime() const noexcept -> float { return DeltaTime; }

            /// 上一次 newFrame() 时的客户区尺寸，单位是像素。
            [[nodiscard]] auto displaySize() const noexcept -> Vector2 { return DisplaySize; }

            /// 窗口处于最小化：它什么都画不出来，宿主可以睡一会儿，而不是空转着跑没人看的帧。
            [[nodiscard]] auto isMinimized() const noexcept -> bool {
                return Window != nullptr && ::IsIconic(Window) != FALSE;
            }

          private:
            /// 为下一次指针离开重新登记 WM_MOUSELEAVE。
            void trackMouseLeave() noexcept {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = Window;
                ::TrackMouseEvent(&tracking);
            }

            [[nodiscard]] static auto readCounter() noexcept -> std::int64_t {
                LARGE_INTEGER counter{};
                ::QueryPerformanceCounter(&counter);
                return static_cast<std::int64_t>(counter.QuadPart);
            }

            HWND Window = nullptr;
            /// 每秒多少个 tick，以及上一帧是从哪个 tick 开始的。
            std::int64_t Frequency = 0;
            std::int64_t PreviousCounter = 0;
            float DeltaTime = 0.0f;
            Vector2 DisplaySize;
        };
    } // namespace Platform
} // namespace ReZeroGui
