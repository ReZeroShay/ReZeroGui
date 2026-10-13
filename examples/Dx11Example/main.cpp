#include <chrono>
#include <cstdio>

#include <ReZeroGui/ReZeroGui.h>
#include <ReZeroGui/Renderer/Dx11Impl.h>
#include <ReZeroGui/Platform/Win32Impl.h>
#include <wrl/client.h>
#include "../Example.h"
namespace {

    struct CAllocator {
      public:
        void *allocate(std::size_t size, [[maybe_unused]] std::size_t alignment) noexcept { return malloc(size); }

        void deallocate(void *ptr) noexcept { free(ptr); }
    };

    constexpr const wchar_t *windowClassName = L"ReZeroGuiDx11Example";

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
    ReZeroGui::Dx11Renderer *dx11renderer{nullptr};
    Microsoft::WRL::ComPtr<IDXGISwapChain> hostSwapChain;
    Microsoft::WRL::ComPtr<ID3D11Device> hostDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> hostDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> hostRenderTargetView;

} // namespace

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (uiContext != nullptr && ReZeroGui::Platform::handleMessage(*uiContext, message, wParam, lParam)) {
        return 0;
    }

    switch (message) {
    case WM_SIZE: {
        const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
        const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
        if (width != 0 && height != 0 && hostSwapChain != nullptr && dx11renderer != nullptr) {
            // Release all back buffer references first (backend RTV + host RTV),
            // then resize, then rebuild them lazily.
            dx11renderer->resize(width, height);
            hostRenderTargetView.Reset();
            hostSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
            if (SUCCEEDED(hostSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
                hostDevice->CreateRenderTargetView(backbuffer.Get(), nullptr, &hostRenderTargetView);
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

    HWND window = ::CreateWindowExW(0, windowClassName, L"ReZeroGui - Direct3D 11 example", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    RECT clientRect{};
    ::GetClientRect(window, &clientRect);
    const auto width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    const auto height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);

    // The host owns the device and swap chain; the backend only borrows them.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deviceContext;
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels,
                                       static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION, &device,
                                       &featureLevel, &deviceContext);
    if (FAILED(result) && (creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        // The debug layer is not installed: retry without it.
        creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels,
                                   static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION, &device,
                                   &featureLevel, &deviceContext);
    }
    if (FAILED(result)) {
        ::MessageBoxW(window, L"Failed to create a Direct3D 11 device.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    // The host owns the device and context; the backend only borrows them.
    hostDevice = device;
    hostDeviceContext = deviceContext;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        ::MessageBoxW(window, L"Failed to reach the DXGI factory.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = window;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (FAILED(factory->CreateSwapChain(device.Get(), &swapChainDesc, &hostSwapChain))) {
        ::MessageBoxW(window, L"Failed to create the swap chain.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    CAllocator cAllocator{};
    ReZeroGui::AllocatorView allocatorView(cAllocator);
    ReZeroGui::Dx11Renderer renderer(allocatorView);
    renderer.initialize(window, device.Get(), hostSwapChain.Get());
    dx11renderer = &renderer;

    ReZeroGui::Context ctx(allocatorView);
    ctx.displaySize(ReZeroGui::Vector2(static_cast<float>(width), static_cast<float>(height)));

    auto &font = ctx.fonts();
    font.loadDefaultFont();
    font.build();

    // The window procedure forwards messages to the context, which is also where
    // WM_SIZE updates the display size used for layout.
    uiContext = &ctx;

    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(hostSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            hostDevice->CreateRenderTargetView(backbuffer.Get(), nullptr, &hostRenderTargetView);
        }
    }

    ::ShowWindow(window, showCommand);
    ::UpdateWindow(window);

    auto previousTime = std::chrono::steady_clock::now();
    bool running = true;

    // The UI is immediate mode, so the host owns every widget's value.
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

        // Hosts that render a 3D scene would do it here; the UI is blended on top
        // of whatever the back buffer already holds, so give it a defined base.
        // Clear colors are 0..1 floats, not 0..255.
        if (hostRenderTargetView != nullptr) {
            const float clearColor[4] = {0.09f, 0.09f, 0.12f, 1.0f};
            hostDeviceContext->ClearRenderTargetView(hostRenderTargetView.Get(), clearColor);
        }
        // 一帧：beginFrame() 开帧，声明窗口/控件，endFrame() 收帧（重排窗口几何、产出
        // DrawData、清掉本帧输入），然后再交给渲染器。
        ctx.beginFrame(deltaTime);
        showExample(ctx);
        ctx.endFrame();
        dx11renderer->renderDrawData(ctx.drawItems());
        hostSwapChain->Present(1u, 0u);
    }

    uiContext = nullptr;
    dx11renderer->shutdown();
    return 0;
}
