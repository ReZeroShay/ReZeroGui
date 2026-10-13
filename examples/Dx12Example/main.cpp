#define NOMINMAX
#include <chrono>
#include <cstdio>

#include <ReZeroGui/ReZeroGui.h>
#include <ReZeroGui/Ui.h>
#include <ReZeroGui/Renderer/Dx12Impl.h>
#include <ReZeroGui/Platform/win32Impl.h>
#include <wrl/client.h>
#include "../Example.h"

namespace {

    struct CAllocator {
      public:
        void *allocate(std::size_t size, [[maybe_unused]] std::size_t alignment) noexcept { return malloc(size); }

        void deallocate(void *ptr) noexcept { free(ptr); }
    };

    constexpr const wchar_t *windowClassName = L"ReZeroGuiDx12Example";

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
    ReZeroGui::Dx12Renderer *dx12renderer{nullptr};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> hostSwapChain;
    Microsoft::WRL::ComPtr<ID3D12Device> hostDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> hostQueue;

} // namespace

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (uiContext != nullptr && ReZeroGui::Platform::handleMessage(*uiContext, message, wParam, lParam)) {
        return 0;
    }

    switch (message) {
    case WM_SIZE: {
        const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
        const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
        if (width != 0 && height != 0 && hostSwapChain != nullptr && dx12renderer != nullptr) {
            // The backend owns the render target views and stops using the old back
            // buffers inside resize(); the swap chain itself is the host's, so the
            // ResizeBuffers call is too.
            dx12renderer->resize(width, height);
            hostSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
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

    HWND window = ::CreateWindowExW(0, windowClassName, L"ReZeroGui - Direct3D 12 example", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    RECT clientRect{};
    ::GetClientRect(window, &clientRect);
    const auto width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    const auto height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);

    // The host owns the device, queue, factory and swap chain; the backend only borrows
    // the first three of those. No debug layer is requested here: the backend is written
    // to be clean under one (the render test runs with it on), but a host that wants it
    // calls D3D12GetDebugInterface/EnableDebugLayer before this point.

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        ::MessageBoxW(window, L"Failed to create the DXGI factory.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        // No hardware device: the software rasterizer is still a real Direct3D 12
        // pipeline, just slower.
        Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))) ||
            FAILED(::D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            ::MessageBoxW(window, L"Failed to create a Direct3D 12 device.", L"ReZeroGui", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
    hostDevice = device;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)))) {
        ::MessageBoxW(window, L"Failed to create a Direct3D 12 command queue.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }
    hostQueue = queue;

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Width = width;
    swapChainDescription.Height = height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &swapChainDescription, nullptr, nullptr,
                                               &swapChain))) {
        ::MessageBoxW(window, L"Failed to create the swap chain.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain.As(&swapChain3))) {
        ::MessageBoxW(window, L"Failed to reach IDXGISwapChain3.", L"ReZeroGui", MB_OK | MB_ICONERROR);
        return 1;
    }
    hostSwapChain = swapChain3;

    CAllocator cAllocator{};
    ReZeroGui::AllocatorView allocatorView(cAllocator);
    ReZeroGui::Dx12Renderer renderer(allocatorView);
    renderer.initialize(device.Get(), queue.Get(), swapChain3.Get());
    dx12renderer = &renderer;

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

        ctx.beginFrame(deltaTime);
        showExample(ctx);
        ctx.endFrame();

        // The backend clears the back buffer, draws the frame and waits for it to
        // finish, so the buffer is back in PRESENT state and the host can present
        // straight away. Present is the host's job; the backend never calls it.
        renderer.renderDrawData(ctx.drawItems());
        hostSwapChain->Present(1u, 0u);
    }

    uiContext = nullptr;
    renderer.shutdown();
    return 0;
}
