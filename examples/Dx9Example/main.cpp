#include <chrono>
#include <cstdio>

#include <ReZeroGui/ReZeroGui.h>
#include <ReZeroGui/Ui.h>
#include <ReZeroGui/Renderer/Dx9Impl.h>
#include <ReZeroGui/Platform/Win32Impl.h>
#include <wrl/client.h>
#include "../Example.h"

namespace {

    struct CAllocator {
      public:
        void *allocate(std::size_t size, [[maybe_unused]] std::size_t alignment) noexcept { return malloc(size); }

        void deallocate(void *ptr) noexcept { free(ptr); }
    };

    constexpr const wchar_t *windowClassName = L"ReZeroGuiDx9Example";

    auto declareDpiAwareness() noexcept {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            using SetProcessDpiAwarenessContextFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
            const auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                reinterpret_cast<void *>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            if (setContext != nullptr) {
                // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Spelled out so the
                // helper builds against SDKs that predate the macro.
                const auto perMonitorV2 = static_cast<DPI_AWARENESS_CONTEXT>(reinterpret_cast<void *>(-4));
                if (setContext(perMonitorV2) != FALSE) {
                    return;
                }
            }
        }
        ::SetProcessDPIAware();
    }
    ReZeroGui::Context *uiContext{nullptr};
    ReZeroGui::Dx9Renderer *dx9renderer{nullptr};
    Microsoft::WRL::ComPtr<IDirect3D9> hostDirect3D;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> hostDevice;
    /// Kept around because IDirect3DDevice9::Reset takes the very same description
    /// back, with only the back buffer size changed in between.
    D3DPRESENT_PARAMETERS hostPresentParameters{};

} // namespace

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (uiContext != nullptr && ReZeroGui::Platform::handleMessage(*uiContext, message, wParam, lParam)) {
        return 0;
    }

    switch (message) {
    case WM_SIZE: {
        const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
        const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
        if (width != 0 && height != 0 && hostDevice != nullptr && dx9renderer != nullptr) {
            // The backend's vertex and index buffers are D3DPOOL_DEFAULT, so they are
            // dropped first: Direct3D 9 refuses to reset the device while one is
            // alive, and they have to be rebuilt at the new size anyway. The device
            // rebuilds its own back buffer in Reset, and every piece of state the
            // backend needs is re-bound on the next frame.
            dx9renderer->resize(width, height);
            hostPresentParameters.BackBufferWidth = width;
            hostPresentParameters.BackBufferHeight = height;
            if (SUCCEEDED(hostDevice->Reset(&hostPresentParameters))) {
                // The UI layout follows the client area; the context learns the new
                // size from the same WM_SIZE through Platform::handleMessage.
            }
        }
        return 0;
    }

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    declareDpiAwareness();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    // IDC_ARROW is an ANSI resource id, so use the ANSI entry point here.
    windowClass.hCursor = ::LoadCursorA(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = windowClassName;
    if (::RegisterClassExW(&windowClass) == 0) {
        return 1;
    }

    HWND window = ::CreateWindowExW(0, windowClassName, L"ReZeroGui - Direct3D 9 example", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    RECT clientRect{};
    ::GetClientRect(window, &clientRect);
    const auto width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    const auto height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);

    hostDirect3D.Attach(::Direct3DCreate9(D3D_SDK_VERSION));
    if (hostDirect3D == nullptr) {
        ::MessageBoxW(window, L"Failed to reach Direct3D 9.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    // The host owns the device, and with it the implicit swap chain the backend
    // borrows; one back buffer plus DISCARD is enough because the UI redraws every
    // pixel of the frame.
    hostPresentParameters.Windowed = TRUE;
    hostPresentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    hostPresentParameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    hostPresentParameters.BackBufferWidth = width;
    hostPresentParameters.BackBufferHeight = height;
    hostPresentParameters.BackBufferCount = 1;
    hostPresentParameters.hDeviceWindow = window;
    hostPresentParameters.EnableAutoDepthStencil = FALSE;
    hostPresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    struct DeviceAttempt {
        D3DDEVTYPE type;
        DWORD flags;
        D3DFORMAT format;
    };
    // Hardware first, then the same adapter with software vertex processing, then
    // the reference rasterizer. X8R8G8B8 is the fallback for an adapter that
    // refuses an alpha channel on the back buffer.
    const DeviceAttempt attempts[] = {
        {D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DFMT_A8R8G8B8},
        {D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DFMT_A8R8G8B8},
        {D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DFMT_X8R8G8B8},
        {D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DFMT_X8R8G8B8},
        {D3DDEVTYPE_REF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DFMT_A8R8G8B8},
    };
    HRESULT deviceResult = E_FAIL;
    for (const DeviceAttempt &attempt : attempts) {
        hostPresentParameters.BackBufferFormat = attempt.format;
        deviceResult = hostDirect3D->CreateDevice(D3DADAPTER_DEFAULT, attempt.type, window, attempt.flags,
                                                  &hostPresentParameters, &hostDevice);
        if (SUCCEEDED(deviceResult)) {
            break;
        }
    }
    if (FAILED(deviceResult)) {
        ::MessageBoxW(window, L"Failed to create a Direct3D 9 device.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    CAllocator cAllocator{};
    ReZeroGui::AllocatorView allocatorView(cAllocator);
    ReZeroGui::Dx9Renderer renderer(allocatorView);
    renderer.initialize(hostDevice.Get());
    dx9renderer = &renderer;

    ReZeroGui::Context ctx(allocatorView);
    ctx.displaySize(ReZeroGui::Vector2(static_cast<float>(width), static_cast<float>(height)));

    // The window procedure forwards messages to the context, which is also where
    // WM_SIZE updates the display size used for layout.
    uiContext = &ctx;

    ::ShowWindow(window, showCommand);
    ::UpdateWindow(window);

    auto previousTime = std::chrono::steady_clock::now();
    bool running = true;

    while (running) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (!running) {
            break;
        }

        const auto currentTime = std::chrono::steady_clock::now();
        const auto deltaTime = std::chrono::duration<float>(currentTime - previousTime).count();
        previousTime = currentTime;

        // Direct3D 9 draws nothing outside BeginScene/EndScene, so the host owns
        // the pair: the clear and the whole UI live inside one scene.
        hostDevice->BeginScene();
        // Hosts that render a 3D scene would do it here; the UI is blended on top
        // of whatever the back buffer already holds, so give it a defined base.
        // Clear colors are 0..1 floats, not 0..255.
        hostDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0.09f, 0.09f, 0.12f, 1.0f), 1.0f, 0);

        ctx.beginFrame(deltaTime);
        showExample(ctx);
        ctx.endFrame();

        renderer.renderDrawData(ctx.drawItems());
        hostDevice->EndScene();
        hostDevice->Present(nullptr, nullptr, nullptr, nullptr);
    }

    uiContext = nullptr;
    renderer.shutdown();
    return 0;
}
