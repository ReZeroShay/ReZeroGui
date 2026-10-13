#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <d3d9.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <ReZeroGui/Allocator.h>
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Draw.h>

namespace ReZeroGui {

    // Direct3D 9 后端。
    //
    // 设备由宿主负责管理：宿主创建 IDirect3DDevice9（隐式交换链也随之创建），
    // 并负责保持场景处于打开状态，即在绘制 UI 之前调用 BeginScene，
    // 并在调用 Present 之前调用 EndScene。
    //
    // 后端不会创建或释放设备、交换链或渲染目标；它只负责设置渲染状态、
    // 上传 UI 所需的数据，并发出绘制调用。
    struct Dx9Renderer {
      public:
        explicit Dx9Renderer(AllocatorView allocator) noexcept : textures(allocator) {}
        auto initialize(IDirect3DDevice9 *device) noexcept {
            REZERO_ASSERT(device);
            Device = device;
            // swapChainHandle is deliberately ignored: Direct3D 9 has no separate
            // swap chain object to borrow, the device owns the implicit one.
            if (!createPipeline()) {
                shutdown();
                return;
            }
            return;
        }
        auto shutdown() noexcept -> void {
            releasePipeline();
            // Not owned by the backend: just forget the host's references.
            Device = nullptr;
        }
        /// Drops the default pool buffers and remembers the size the host is about to
        /// render at. Call this before IDirect3DDevice9::Reset: Reset fails while any
        /// D3DPOOL_DEFAULT resource is alive, and the buffers has to be recreated at
        /// the new back buffer size anyway. A host that never resets the device (an
        /// offscreen one, for instance) does not need to call this at all.
        void resize(std::uint32_t width, std::uint32_t height) noexcept {
            releaseBuffers();
            targetWidth = width;
            targetHeight = height;
        }

        void newFrame() noexcept {
            // Nothing to prepare: state is fully set up per renderDrawData call.
        }
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

            // Managed pool: the pixels live in system memory and follow the texture
            // into the device after a Reset, which is what the UI needs - its
            // textures outlive a resize and must never be asked for again.
            IDirect3DTexture9 *texture = nullptr;
            if (FAILED(
                    Device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
                return false;
            }

            record->texture = texture;
            record->width = width;
            record->height = height;
            if (rgba != nullptr && !uploadPixels(*record, rgba)) {
                // A texture that never got its pixels is worse than no texture at
                // all, because the slot would report itself ready from now on.
                releaseRecord(*record);
                return false;
            }
            return true;
        }

        /// Re-uploads the pixels of an existing texture. The texture must have been
        /// created with the same dimensions.
        bool updateTexture(TextureId handle, const std::uint8_t *rgba) noexcept {
            TextureRecord *record = textureFor(handle);
            if (record == nullptr || record->texture == nullptr || Device == nullptr || rgba == nullptr) {
                return false;
            }
            if (record->width == 0 || record->height == 0) {
                return false;
            }
            return uploadPixels(*record, rgba);
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
            if (Device == nullptr || !data.valid) {
                return;
            }

            // Textures are published before the geometry that samples them. The
            // queue only grows when the UI creates or destroys a texture, so
            // replaying it every frame is cheap and also restores every handle
            // after a device reset.
            applyResourceCommands(data.resourceCommands);

            // The render target belongs to the host, so its size is asked for rather
            // than remembered: the back buffer changes under Reset(), and a host that
            // renders offscreen never announces its target through resize(). What
            // resize() stored is only the fallback for a device with no target yet.
            if (!queryRenderTargetSize()) {
                renderTargetWidth = targetWidth != 0 ? targetWidth : static_cast<std::uint32_t>(data.displaySize.x);
                renderTargetHeight = targetHeight != 0 ? targetHeight : static_cast<std::uint32_t>(data.displaySize.y);
            }

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
            IDirect3DTexture9 *texture{nullptr};
            std::uint32_t width{0};
            std::uint32_t height{0};
        };

        struct VertexConstants {
            float projection[16];
        };

        /// Releases the device object of one slot and leaves it empty. Safe to call
        /// on an already empty slot.
        static void releaseRecord(TextureRecord &record) noexcept {
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

        /// Copies one RGBA8 image into a texture level.
        ///
        /// The source is the library's byte order (red in byte 0) and it is stored
        /// unchanged; the R/B swap is done by the pixel shader, see compileShaders.
        static auto uploadPixels(TextureRecord &record, const std::uint8_t *rgba) noexcept -> bool {
            D3DLOCKED_RECT locked{};
            // No D3DLOCK_DISCARD: the texture is managed pool, and that flag is only
            // valid for dynamic default pool resources.
            if (FAILED(record.texture->LockRect(0, &locked, nullptr, 0))) {
                return false;
            }
            const std::size_t rowBytes = static_cast<std::size_t>(record.width) * 4;
            for (std::uint32_t row = 0; row < record.height; ++row) {
                auto *destination = static_cast<std::uint8_t *>(locked.pBits) +
                                    static_cast<std::size_t>(row) * static_cast<std::size_t>(locked.Pitch);
                std::memcpy(destination, rgba + static_cast<std::size_t>(row) * rowBytes, rowBytes);
            }
            record.texture->UnlockRect(0);
            return true;
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
            // All four channels are 255, so this one is independent of the swizzle
            // the pixel shader applies to real textures.
            constexpr D3DCOLOR White = 0xFFFFFFFFu;

            if (FAILED(Device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &whiteTexture, nullptr))) {
                return false;
            }
            D3DLOCKED_RECT locked{};
            if (FAILED(whiteTexture->LockRect(0, &locked, nullptr, 0))) {
                whiteTexture->Release();
                whiteTexture = nullptr;
                return false;
            }
            *static_cast<D3DCOLOR *>(locked.pBits) = White;
            whiteTexture->UnlockRect(0);
            return true;
        }

        /// Size of whatever the device currently renders into, or false while there
        /// is no render target to ask. Polling is what keeps the scissor rectangle
        /// and the viewport correct across a host driven Reset.
        auto queryRenderTargetSize() noexcept -> bool {
            IDirect3DSurface9 *target = nullptr;
            if (FAILED(Device->GetRenderTarget(0, &target)) || target == nullptr) {
                return false;
            }
            D3DSURFACE_DESC description{};
            const bool valid =
                SUCCEEDED(target->GetDesc(&description)) && description.Width != 0 && description.Height != 0;
            target->Release();
            if (!valid) {
                return false;
            }
            renderTargetWidth = description.Width;
            renderTargetHeight = description.Height;
            return true;
        }

        auto compileShaders() noexcept {
            static constexpr const char *VertexSource = R"(
                float4x4 ProjectionMatrix : register(c0);

                struct VertexInput
                {
                    float2 position : POSITION;
                    float2 uv       : TEXCOORD0;
                    float4 color    : COLOR0;
                };

                struct PixelInput
                {
                    float4 position : POSITION;
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
                    float4 position : POSITION;
                    float4 color    : COLOR0;
                    float2 uv       : TEXCOORD0;
                };

                sampler2D uiSampler : register(s0);
                // Half a texel of the texture bound to this draw command; see the
                // sampling note in pixelMain.
                float4 uiTexelSize : register(c0);

                float4 pixelMain(PixelInput input) : COLOR
                {
                    // Direct3D 9 puts a texel's center half a texel away from where
                    // the Direct3D 10 style UVs the UI produces expect it, so
                    // sampling on the UVs as they arrive would land on texel
                    // corners: a bilinear average of four neighbours, i.e. text
                    // that is softer and dimmer than the Direct3D 11 backend draws.
                    // The half texel shift is what makes the two backends produce
                    // the same pixels.
                    // The UI layer is RGBA8 with red in byte 0, while both
                    // D3DCOLOR (the vertex color) and D3DFMT_A8R8G8B8 (the texture)
                    // keep blue in byte 0. The shader is where that is corrected: a
                    // CPU side conversion would have to walk every uploaded atlas and
                    // every vertex color, and A8R8G8B8 is the one 32 bit format every
                    // Direct3D 9 device is guaranteed to support.
                    float4 sampled = tex2D(uiSampler, input.uv + uiTexelSize.xy);
                    return input.color.bgra * sampled.bgra;
                }
                )";
            UINT flags = 0;
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;

            // Each compile hands back an error blob even on success (it is empty
            // then); every one of them has to be released.
            ID3DBlob *vertexBlob = nullptr;
            ID3DBlob *pixelBlob = nullptr;
            ID3DBlob *errorBlob = nullptr;
            if (FAILED(D3DCompile(VertexSource, std::strlen(VertexSource), nullptr, nullptr, nullptr, "vertexMain",
                                  "vs_3_0", flags, 0, &vertexBlob, &errorBlob))) {
                releaseBlob(errorBlob);
                releaseBlob(vertexBlob);
                return false;
            }
            releaseBlob(errorBlob);

            if (FAILED(D3DCompile(PixelSource, std::strlen(PixelSource), nullptr, nullptr, nullptr, "pixelMain",
                                  "ps_3_0", flags, 0, &pixelBlob, &errorBlob))) {
                releaseBlob(errorBlob);
                releaseBlob(pixelBlob);
                releaseBlob(vertexBlob);
                return false;
            }
            releaseBlob(errorBlob);

            // Direct3D 9 takes the bytecode directly, so unlike Direct3D 11 the
            // blobs are not needed past this point: the vertex declaration below is
            // built from the C++ struct rather than from the shader's signature.
            const HRESULT vertexResult =
                Device->CreateVertexShader(static_cast<const DWORD *>(vertexBlob->GetBufferPointer()), &vertexShader);
            const HRESULT pixelResult =
                Device->CreatePixelShader(static_cast<const DWORD *>(pixelBlob->GetBufferPointer()), &pixelShader);
            releaseBlob(vertexBlob);
            releaseBlob(pixelBlob);

            return SUCCEEDED(vertexResult) && SUCCEEDED(pixelResult);
        }
        auto createPipeline() noexcept -> bool {
            // Position, uv and color, all in one stream. The offsets are asserted
            // below: the declaration is what the GPU sees, so a changed DrawVertex
            // has to fail the build rather than silently shift a channel.
            const D3DVERTEXELEMENT9 elements[] = {{0, static_cast<WORD>(offsetof(DrawVertex, position)),
                                                   D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                                  {0, static_cast<WORD>(offsetof(DrawVertex, uv)), D3DDECLTYPE_FLOAT2,
                                                   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                                  {0, static_cast<WORD>(offsetof(DrawVertex, pixel)),
                                                   D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                                  D3DDECL_END()};
            static_assert(offsetof(DrawVertex, position) == 0);
            static_assert(offsetof(DrawVertex, uv) == 8);
            static_assert(offsetof(DrawVertex, pixel) == 16);
            static_assert(sizeof(DrawVertex) == 20);

            if (FAILED(Device->CreateVertexDeclaration(elements, &vertexDeclaration))) {
                return false;
            }
            if (!compileShaders()) {
                return false;
            }

            // Untextured primitives sample this instead of needing their own
            // pipeline variant.
            return createWhiteTexture();
        }
        void releaseBuffers() noexcept {
            if (vertexBuffer != nullptr) {
                vertexBuffer->Release();
                vertexBuffer = nullptr;
            }
            if (indexBuffer != nullptr) {
                indexBuffer->Release();
                indexBuffer = nullptr;
            }
            vertexCapacity = 0;
            indexCapacity = 0;
        }
        void releasePipeline() noexcept {
            // Every texture the UI asked for lives in the slot table; releasing
            // the pipeline releases all of them.
            for (TextureRecord &record : textures) {
                releaseRecord(record);
            }
            textures.clear();

            if (whiteTexture != nullptr) {
                whiteTexture->Release();
                whiteTexture = nullptr;
            }

            releaseBuffers();

            // Release each COM pointer exactly once and forget it; any of them
            // may still be null when the pipeline was only partially created.
            auto releaseAndClear = [](auto &comPointer) noexcept {
                if (comPointer != nullptr) {
                    comPointer->Release();
                    comPointer = nullptr;
                }
            };
            releaseAndClear(vertexDeclaration);
            releaseAndClear(vertexShader);
            releaseAndClear(pixelShader);
        }

        auto ensureBuffers(std::uint32_t vertexCount, std::uint32_t indexCount) noexcept -> bool {
            if (vertexCount > vertexCapacity) {
                if (vertexBuffer != nullptr) {
                    vertexBuffer->Release();
                    vertexBuffer = nullptr;
                }
                const std::uint32_t capacity = vertexCount + vertexCount / 2 + 5000;
                if (FAILED(Device->CreateVertexBuffer(capacity * sizeof(DrawVertex),
                                                      D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                                      &vertexBuffer, nullptr))) {
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
                if (FAILED(Device->CreateIndexBuffer(capacity * sizeof(DrawIndex),
                                                     D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                                     D3DPOOL_DEFAULT, &indexBuffer, nullptr))) {
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

            // One constant register set, so unlike Direct3D 11 there is no buffer
            // to map: the matrix is handed straight to the vertex shader.
            Device->SetVertexShaderConstantF(0, constants.projection, 4);
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

            // Both buffers are dynamic and written in full every frame, so DISCARD
            // is what keeps the driver from waiting for the previous frame's draws.
            void *vertices = nullptr;
            if (FAILED(vertexBuffer->Lock(0, 0, &vertices, D3DLOCK_DISCARD))) {
                return;
            }
            std::memcpy(vertices, list.currentVertices().data(), vertexCount * sizeof(DrawVertex));
            vertexBuffer->Unlock();

            void *indices = nullptr;
            if (FAILED(indexBuffer->Lock(0, 0, &indices, D3DLOCK_DISCARD))) {
                return;
            }
            std::memcpy(indices, list.currentIndices().data(), indexCount * sizeof(DrawIndex));
            indexBuffer->Unlock();

            bindPipeline();
            for (const DrawCommand &command : list.currentCommands()) {
                if (command.indexCount == 0) {
                    continue;
                }

                const Rect clip = command.clipRect.intersected(
                    Rect(0.0f, 0.0f, static_cast<float>(renderTargetWidth), static_cast<float>(renderTargetHeight)));
                if (clip.isEmpty()) {
                    continue;
                }

                // Direct3D 9 validates the scissor rectangle against the render
                // target, so it is the intersected one that gets set, never the
                // UI's clip as it came in.
                const RECT scissor{static_cast<LONG>(clip.x), static_cast<LONG>(clip.y), static_cast<LONG>(clip.maxX()),
                                   static_cast<LONG>(clip.maxY())};
                Device->SetScissorRect(&scissor);

                IDirect3DTexture9 *texture = whiteTexture;
                std::uint32_t texelWidth = 1;
                std::uint32_t texelHeight = 1;
                if (TextureRecord *record = textureFor(command.textureId);
                    record != nullptr && record->texture != nullptr) {
                    texture = record->texture;
                    texelWidth = record->width;
                    texelHeight = record->height;
                }
                Device->SetTexture(0, texture);

                // The half texel correction the pixel shader applies depends on the
                // texture, so it belongs to the command rather than to bindPipeline.
                const float halfTexel[4] = {0.5f / static_cast<float>(texelWidth),
                                            0.5f / static_cast<float>(texelHeight), 0.0f, 0.0f};
                Device->SetPixelShaderConstantF(0, halfTexel, 1);

                Device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, vertexCount, command.indexOffset,
                                             command.indexCount / 3);
            }
        }
        auto bindPipeline() noexcept -> void {
            Device->SetVertexDeclaration(vertexDeclaration);
            Device->SetStreamSource(0, vertexBuffer, 0, sizeof(DrawVertex));
            Device->SetIndices(indexBuffer);
            Device->SetVertexShader(vertexShader);
            Device->SetPixelShader(pixelShader);

            Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            Device->SetRenderState(D3DRS_SRCBLEND, static_cast<DWORD>(D3DBLEND_SRCALPHA));
            Device->SetRenderState(D3DRS_DESTBLEND, static_cast<DWORD>(D3DBLEND_INVSRCALPHA));
            Device->SetRenderState(D3DRS_BLENDOP, static_cast<DWORD>(D3DBLENDOP_ADD));
            Device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            Device->SetRenderState(D3DRS_CULLMODE, static_cast<DWORD>(D3DCULL_NONE));
            // The UI is flat and drawn in stream order; a depth buffer would only
            // let a widget hide another one by accident.
            Device->SetRenderState(D3DRS_ZENABLE, FALSE);
            Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            // Lighting and fog would both multiply the vertex color behind our back.
            Device->SetRenderState(D3DRS_LIGHTING, FALSE);
            Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            Device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
            // A widget straddling a texel edge samples the atlas, so filtering is
            // linear; clamping keeps a quad at the atlas border from wrapping.
            Device->SetSamplerState(0, D3DSAMP_MINFILTER, static_cast<DWORD>(D3DTEXF_LINEAR));
            Device->SetSamplerState(0, D3DSAMP_MAGFILTER, static_cast<DWORD>(D3DTEXF_LINEAR));
            Device->SetSamplerState(0, D3DSAMP_MIPFILTER, static_cast<DWORD>(D3DTEXF_NONE));
            Device->SetSamplerState(0, D3DSAMP_ADDRESSU, static_cast<DWORD>(D3DTADDRESS_CLAMP));
            Device->SetSamplerState(0, D3DSAMP_ADDRESSV, static_cast<DWORD>(D3DTADDRESS_CLAMP));

            // The render target is the host's, so the viewport is taken from it
            // rather than assumed from the UI's display size.
            D3DVIEWPORT9 viewport{};
            viewport.Width = renderTargetWidth;
            viewport.Height = renderTargetHeight;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            Device->SetViewport(&viewport);
        }

        IDirect3DDevice9 *Device{nullptr};

        IDirect3DVertexDeclaration9 *vertexDeclaration{nullptr};
        IDirect3DVertexShader9 *vertexShader{nullptr};
        IDirect3DPixelShader9 *pixelShader{nullptr};
        IDirect3DVertexBuffer9 *vertexBuffer{nullptr};
        IDirect3DIndexBuffer9 *indexBuffer{nullptr};
        /// 1x1 white used by untextured primitives. Owned directly rather than
        /// through the handle table, which only carries UI requested textures.
        IDirect3DTexture9 *whiteTexture{nullptr};

        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;
        /// Size the host last announced through resize(), used only while the device
        /// has no render target to measure.
        std::uint32_t targetWidth = 0;
        std::uint32_t targetHeight = 0;
        /// Size of the render target the last frame was measured against.
        std::uint32_t renderTargetWidth = 0;
        std::uint32_t renderTargetHeight = 0;
        ArrayList<TextureRecord> textures;
    };
} // namespace ReZeroGui
