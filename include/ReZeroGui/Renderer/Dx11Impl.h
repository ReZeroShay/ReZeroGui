#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <ReZeroGui/Allocator.h>
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Draw.h>

namespace ReZeroGui {

    struct Dx11Renderer {
      public:
        Dx11Renderer(AllocatorView allocator) : textures(allocator) {}
        void initialize(ID3D11Device *dev, IDXGISwapChain *swap, ID3D11DeviceContext *context) noexcept {
            REZERO_ASSERT(dev != nullptr);
            REZERO_ASSERT(swap != nullptr);
            REZERO_ASSERT(context != nullptr);

            Device = dev;
            SwapChain = swap;
            Context = context;
            if (!createRenderTarget() || !createPipeline()) {
                shutdown();
                return;
            }
            return;
        }
        void shutdown() noexcept {
            releasePipeline();
            releaseRenderTarget();
            // Not owned by the backend: just forget the host's references.
            Device = nullptr;
            Context = nullptr;
            SwapChain = nullptr;
        }
        /// Drops the render target view so the host can resize its swap chain
        /// buffers. Call this before IDXGISwapChain::ResizeBuffers; the view is
        /// recreated from the new back buffer on the next renderDrawData.
        void resize(std::uint32_t, std::uint32_t) noexcept { releaseRenderTarget(); }

        /// Creates the device texture behind a UI handle.
        ///
        /// Called with handles handed out by Context::createTexture. Creating a
        /// handle that is already live is a no-op: the UI keeps its resource
        /// commands around for the lifetime of the context, so the renderer sees
        /// the same creates every frame and must not rebuild the texture.
        auto ensureTexture(TextureId handle, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t *rgba) noexcept -> bool {
            if (Device == nullptr || width == 0 || height == 0) {
                return false;
            }

            TextureRecord *record = ensureRecord(handle);
            if (record == nullptr) {
                return false;
            }
            if (record->texture != nullptr) {
                return true;
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags = 0;

            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = rgba;
            initialData.SysMemPitch = width * 4;

            ID3D11Texture2D *texture = nullptr;
            if (FAILED(Device->CreateTexture2D(&description, rgba != nullptr ? &initialData : nullptr, &texture))) {
                return false;
            }

            ID3D11ShaderResourceView *view = nullptr;
            if (FAILED(Device->CreateShaderResourceView(texture, nullptr, &view))) {
                // The texture was created but is useless without a view; the view
                // is what the slot hands to the pipeline, so drop both.
                texture->Release();
                return false;
            }

            record->texture = texture;
            record->view = view;
            record->width = width;
            record->height = height;
            return true;
        }

        /// Re-uploads the pixels of an existing texture. The texture must have been
        /// created with the same dimensions.
        bool updateTexture(TextureId handle, const std::uint8_t *rgba) noexcept {
            TextureRecord *record = textureFor(handle);
            if (record == nullptr || record->texture == nullptr || Context == nullptr || rgba == nullptr) {
                return false;
            }
            if (record->width == 0 || record->height == 0) {
                return false;
            }
            Context->UpdateSubresource(record->texture, 0, nullptr, rgba, record->width * 4, 0);
            return true;
        }

        /// Releases the device texture behind `handle`, leaving the slot empty.
        /// Destroying an unknown or already destroyed handle is a no-op.
        void destroyTexture(TextureId handle) noexcept {
            TextureRecord *record = textureFor(handle);
            if (record == nullptr) {
                return;
            }
            releaseRecord(*record);
        }

        /// Replays the UI context's texture lifetime requests, in order.
        void applyResourceCommands(Span<const ResourceCmd> commands) noexcept {
            for (const ResourceCmd &command : commands) {
                switch (command.type) {
                case ResourceCmdType::CreateTexture:
                    ensureTexture(command.handle, command.desc.width, command.desc.height, command.pixels);
                    break;
                case ResourceCmdType::UpdateTexture:
                    updateTexture(command.handle, command.pixels);
                    break;
                case ResourceCmdType::DestroyTexture:
                    destroyTexture(command.handle);
                    break;
                }
            }
        }

        auto renderDrawData(const DrawData &data) noexcept {
            if (Device == nullptr || Context == nullptr || !data.valid) {
                return;
            }
            if (renderTargetView == nullptr && !createRenderTarget()) {
                return;
            }

            // Textures are published before the geometry that samples them. The
            // queue only grows when the UI creates or destroys a texture, so
            // replaying it every frame is cheap and also restores every handle
            // after a device or back buffer reset.
            applyResourceCommands(data.resourceCommands);

            setProjection(static_cast<float>(data.displaySize.x), static_cast<float>(data.displaySize.y));

            if (data.background != nullptr) {
                renderList(*data.background);
            }
            if (data.foreground != nullptr) {
                renderList(*data.foreground);
            }
            if (data.overlay != nullptr) {
                renderList(*data.overlay);
            }
        }

      private:
        struct TextureRecord {
            ID3D11Texture2D *texture{nullptr};
            ID3D11ShaderResourceView *view{nullptr};
            std::uint32_t width{0};
            std::uint32_t height{0};
        };

        struct VertexConstants {
            float projection[16];
        };

        /// Releases the device objects of one slot and leaves it empty. Safe to
        /// call on an already empty slot.
        static void releaseRecord(TextureRecord &record) noexcept {
            if (record.view != nullptr) {
                record.view->Release();
                record.view = nullptr;
            }
            if (record.texture != nullptr) {
                record.texture->Release();
                record.texture = nullptr;
            }
            record.width = 0;
            record.height = 0;
        }

        /// Slot lookup for command processing: grows the table so that a handle
        /// always maps to the same slot. Handles are 1 based; 0 and
        /// InvalidTexture are not valid handles.
        auto ensureRecord(TextureId handle) noexcept -> TextureRecord * {
            if (handle == InvalidTexture || handle == 0) {
                return nullptr;
            }
            const std::size_t index = static_cast<std::size_t>(handle - 1);
            while (textures.size() <= index) {
                TextureRecord record;
                if (!textures.push_back(record)) {
                    return nullptr;
                }
            }
            return &textures[index];
        }

        /// Slot lookup for drawing: never grows the table.
        auto textureFor(TextureId handle) noexcept -> TextureRecord * {
            if (handle == InvalidTexture || handle == 0) {
                return nullptr;
            }
            const std::size_t index = static_cast<std::size_t>(handle - 1);
            return index < textures.size() ? &textures[index] : nullptr;
        }

        static void releaseBlob(ID3DBlob *&blob) noexcept {
            if (blob != nullptr) {
                blob->Release();
                blob = nullptr;
            }
        }

        /// Builds the 1x1 opaque white texture that untextured primitives sample,
        /// so the pixel shader can stay a single multiply for both cases.
        auto createWhiteTexture() noexcept -> bool {
            static const std::uint8_t White[4] = {255, 255, 255, 255};

            D3D11_TEXTURE2D_DESC description{};
            description.Width = 1;
            description.Height = 1;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = White;
            initialData.SysMemPitch = 4;

            ID3D11Texture2D *texture = nullptr;
            if (FAILED(Device->CreateTexture2D(&description, &initialData, &texture))) {
                return false;
            }
            const HRESULT result = Device->CreateShaderResourceView(texture, nullptr, &whiteView);
            texture->Release();
            return SUCCEEDED(result);
        }

        auto createRenderTarget() -> bool {
            ID3D11Texture2D *backbuffer;
            if (FAILED(SwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
                return false;
            }
            if (FAILED(Device->CreateRenderTargetView(backbuffer, nullptr, &renderTargetView))) {
                backbuffer->Release();
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            backbuffer->GetDesc(&description);
            backbufferWidth = description.Width;
            backbufferHeight = description.Height;
            backbuffer->Release();
            return true;
        }
        auto releaseRenderTarget() noexcept -> void {
            if (renderTargetView != nullptr) {
                renderTargetView->Release();
                renderTargetView = nullptr;
            }
        }
        auto compileShaders() noexcept {
            static constexpr const char *VertexSource = R"(
                cbuffer VertexConstants : register(b0)
                {
                    float4x4 ProjectionMatrix;
                };

                struct VertexInput
                {
                    float2 position : POSITION;
                    float2 uv       : TEXCOORD0;
                    float4 color    : COLOR0;
                };

                struct PixelInput
                {
                    float4 position : SV_POSITION;
                    float4 color    : COLOR0;
                    float2 uv       : TEXCOORD0;
                };

                PixelInput vertexMain(VertexInput input)
                {
                    PixelInput output;
                    output.position = mul(ProjectionMatrix, float4(input.position, 0.0f, 1.0f));
                    output.color = input.color;
                    output.uv = input.uv;
                    return output;
                }
                )";

            static constexpr const char *PixelSource = R"(
                struct PixelInput
                {
                    float4 position : SV_POSITION;
                    float4 color    : COLOR0;
                    float2 uv       : TEXCOORD0;
                };

                Texture2D uiTexture : register(t0);
                SamplerState uiSampler : register(s0);

                float4 pixelMain(PixelInput input) : SV_Target
                {
                    float4 sampled = uiTexture.Sample(uiSampler, input.uv);
                    return input.color * sampled;
                }
                )";
            UINT flags = 0;
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;

            // Each compile hands back an error blob even on success (it is empty
            // then); every one of them has to be released.
            ID3DBlob *errorBlob = nullptr;
            if (FAILED(D3DCompile(VertexSource, std::strlen(VertexSource), nullptr, nullptr, nullptr, "vertexMain",
                                  "vs_5_0", flags, 0, &vertexBlob, &errorBlob))) {
                releaseBlob(errorBlob);
                return false;
            }
            releaseBlob(errorBlob);

            ID3DBlob *pixelBlob = nullptr;
            if (FAILED(D3DCompile(PixelSource, std::strlen(PixelSource), nullptr, nullptr, nullptr, "pixelMain",
                                  "ps_5_0", flags, 0, &pixelBlob, &errorBlob))) {
                releaseBlob(errorBlob);
                return false;
            }
            releaseBlob(errorBlob);

            if (FAILED(Device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr,
                                                  &vertexShader))) {
                pixelBlob->Release();
                return false;
            }
            if (FAILED(Device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr,
                                                 &pixelShader))) {
                pixelBlob->Release();
                return false;
            }
            // vertexBlob is still needed to build the input layout; it is released
            // with the rest of the pipeline.
            pixelBlob->Release();
            return true;
        }
        auto createPipeline() noexcept -> bool {
            if (!compileShaders()) {
                return false;
            }

            const D3D11_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(DrawVertex, position),
                 D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(DrawVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(DrawVertex, pixel), D3D11_INPUT_PER_VERTEX_DATA,
                 0},
            };
            static_assert(offsetof(DrawVertex, position) == 0);
            static_assert(offsetof(DrawVertex, uv) == 8);
            static_assert(offsetof(DrawVertex, pixel) == 16);
            static_assert(sizeof(DrawVertex) == 20);

            if (FAILED(Device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
                                                 vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                 &inputLayout))) {
                return false;
            }

            D3D11_BUFFER_DESC constantDescription{};
            constantDescription.ByteWidth = static_cast<UINT>((sizeof(VertexConstants) + 15) & ~15u);
            constantDescription.Usage = D3D11_USAGE_DYNAMIC;
            constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(Device->CreateBuffer(&constantDescription, nullptr, &constantBuffer))) {
                return false;
            }

            D3D11_BLEND_DESC blend{};
            blend.AlphaToCoverageEnable = FALSE;
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(Device->CreateBlendState(&blend, &blendState))) {
                return false;
            }

            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.FillMode = D3D11_FILL_SOLID;
            rasterizer.CullMode = D3D11_CULL_NONE;
            rasterizer.ScissorEnable = TRUE;
            rasterizer.DepthClipEnable = TRUE;
            rasterizer.MultisampleEnable = TRUE;
            rasterizer.AntialiasedLineEnable = FALSE;
            if (FAILED(Device->CreateRasterizerState(&rasterizer, &rasterizerState))) {
                return false;
            }

            D3D11_DEPTH_STENCIL_DESC depthStencil{};
            depthStencil.DepthEnable = FALSE;
            depthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depthStencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
            depthStencil.StencilEnable = FALSE;
            depthStencil.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            depthStencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            depthStencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            depthStencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            depthStencil.BackFace = depthStencil.FrontFace;
            if (FAILED(Device->CreateDepthStencilState(&depthStencil, &depthStencilState))) {
                return false;
            }

            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            if (FAILED(Device->CreateSamplerState(&sampler, &samplerState))) {
                return false;
            }

            // Untextured primitives sample this instead of needing their own
            // pipeline variant.
            return createWhiteTexture();
        }
        void releasePipeline() noexcept {
            // Every texture the UI asked for lives in the slot table; releasing
            // the pipeline releases all of them.
            for (TextureRecord &record : textures) {
                releaseRecord(record);
            }
            textures.clear();

            if (whiteView != nullptr) {
                whiteView->Release();
                whiteView = nullptr;
            }

            // Release each COM pointer exactly once and forget it; any of them
            // may still be null when the pipeline was only partially created.
            auto releaseAndClear = [](auto &comPointer) noexcept {
                if (comPointer != nullptr) {
                    comPointer->Release();
                    comPointer = nullptr;
                }
            };
            releaseAndClear(vertexBuffer);
            releaseAndClear(indexBuffer);
            releaseAndClear(constantBuffer);
            releaseAndClear(inputLayout);
            releaseAndClear(vertexShader);
            releaseAndClear(pixelShader);
            releaseAndClear(blendState);
            releaseAndClear(rasterizerState);
            releaseAndClear(depthStencilState);
            releaseAndClear(samplerState);
            releaseAndClear(vertexBlob);
            vertexCapacity = 0;
            indexCapacity = 0;
        }

        auto ensureBuffers(std::uint32_t vertexCount, std::uint32_t indexCount) noexcept -> bool {
            if (vertexCount > vertexCapacity) {
                if (vertexBuffer != nullptr) {
                    vertexBuffer->Release();
                    vertexBuffer = nullptr;
                }
                const std::uint32_t capacity = vertexCount + vertexCount / 2 + 5000;
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = capacity * sizeof(DrawVertex);
                description.Usage = D3D11_USAGE_DYNAMIC;
                description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(Device->CreateBuffer(&description, nullptr, &vertexBuffer))) {
                    return false;
                }
                vertexCapacity = capacity;
            }

            if (indexCount > indexCapacity) {
                if (indexBuffer != nullptr) {
                    indexBuffer->Release();
                    indexBuffer = nullptr;
                }
                const std::uint32_t capacity = indexCount + indexCount / 2 + 10000;
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = capacity * sizeof(DrawIndex);
                description.Usage = D3D11_USAGE_DYNAMIC;
                description.BindFlags = D3D11_BIND_INDEX_BUFFER;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(Device->CreateBuffer(&description, nullptr, &indexBuffer))) {
                    return false;
                }
                indexCapacity = capacity;
            }
            return true;
        }

        auto setProjection(float displayWidth, float displayHeight) noexcept -> void {
            const float left = 0.0f;
            const float right = displayWidth;
            const float top = 0.0f;
            const float bottom = displayHeight;

            VertexConstants constants{};
            constants.projection[0] = 2.0f / (right - left);
            constants.projection[5] = 2.0f / (top - bottom);
            constants.projection[10] = 0.5f;
            constants.projection[12] = (right + left) / (left - right);
            constants.projection[13] = (top + bottom) / (bottom - top);
            constants.projection[14] = 0.5f;
            constants.projection[15] = 1.0f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(Context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.pData, &constants, sizeof(constants));
                Context->Unmap(constantBuffer, 0);
            }
        }

        auto renderList(const DrawList &list) noexcept -> void {
            if (list.isEmpty()) {
                return;
            }

            const std::uint32_t vertexCount = list.vertexCount();
            const std::uint32_t indexCount = list.indexCount();
            if (!ensureBuffers(vertexCount, indexCount)) {
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(Context->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            memcpy(mapped.pData, list.currentVertices().data(), vertexCount * sizeof(DrawVertex));
            Context->Unmap(vertexBuffer, 0);

            if (FAILED(Context->Map(indexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            memcpy(mapped.pData, list.currentIndices().data(), indexCount * sizeof(DrawIndex));
            Context->Unmap(indexBuffer, 0);

            bindPipeline();
            for (const DrawCommand &command : list.currentCommands()) {
                if (command.indexCount == 0) {
                    continue;
                }

                const Rect clip = command.clipRect.intersected(
                    Rect(0.0f, 0.0f, static_cast<float>(backbufferWidth), static_cast<float>(backbufferHeight)));
                if (clip.isEmpty()) {
                    continue;
                }

                const D3D11_RECT scissor{static_cast<LONG>(clip.x), static_cast<LONG>(clip.y),
                                         static_cast<LONG>(clip.maxX()), static_cast<LONG>(clip.maxY())};
                Context->RSSetScissorRects(1, &scissor);

                ID3D11ShaderResourceView *view = whiteView;
                if (TextureRecord *record = textureFor(command.textureId);
                    record != nullptr && record->view != nullptr) {
                    view = record->view;
                }
                Context->PSSetShaderResources(0, 1, &view);

                Context->DrawIndexed(command.indexCount, command.indexOffset, 0);
            }
        }
        auto bindPipeline() noexcept -> void {
            const UINT stride = sizeof(DrawVertex);
            const UINT offset = 0;
            Context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
            Context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            Context->IASetInputLayout(inputLayout);
            Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Context->VSSetShader(vertexShader, nullptr, 0);
            Context->VSSetConstantBuffers(0, 1, &constantBuffer);
            Context->PSSetShader(pixelShader, nullptr, 0);
            Context->PSSetSamplers(0, 1, &samplerState);
            Context->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
            Context->OMSetDepthStencilState(depthStencilState, 0);
            Context->RSSetState(rasterizerState);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(backbufferWidth);
            viewport.Height = static_cast<float>(backbufferHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            Context->RSSetViewports(1, &viewport);

            ID3D11RenderTargetView *target = renderTargetView;
            Context->OMSetRenderTargets(1, &target, nullptr);
        }

        HWND Hwnd{nullptr};
        ID3D11Device *Device{nullptr};
        ID3D11DeviceContext *Context{nullptr};
        IDXGISwapChain *SwapChain{nullptr};

        ID3D11RenderTargetView *renderTargetView{nullptr};

        ID3DBlob *vertexBlob{nullptr};
        ID3D11VertexShader *vertexShader{nullptr};
        ID3D11PixelShader *pixelShader{nullptr};
        ID3D11InputLayout *inputLayout{nullptr};
        ID3D11Buffer *constantBuffer{nullptr};
        ID3D11Buffer *vertexBuffer{nullptr};
        ID3D11Buffer *indexBuffer{nullptr};
        ID3D11BlendState *blendState{nullptr};
        ID3D11RasterizerState *rasterizerState{nullptr};
        ID3D11DepthStencilState *depthStencilState{nullptr};
        ID3D11SamplerState *samplerState{nullptr};
        /// 1x1 white used by untextured primitives. Owned directly rather than
        /// through the handle table, which only carries UI requested textures.
        ID3D11ShaderResourceView *whiteView{nullptr};

        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;
        std::uint32_t backbufferWidth = 0;
        std::uint32_t backbufferHeight = 0;
        ArrayList<TextureRecord> textures;
    };
} // namespace ReZeroGui
