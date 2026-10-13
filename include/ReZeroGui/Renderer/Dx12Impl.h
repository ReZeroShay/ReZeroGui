#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <ReZeroGui/Allocator.h>
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/Draw.h>

namespace ReZeroGui {

    // Direct3D 12 后端。
    //
    // 宿主负责拥有和管理设备、命令队列及交换链；后端仅借用这三者，
    // 不会释放它们所占用的资源。
    //
    // 后端自身拥有的资源包括：根签名、管线状态、描述符堆、后台缓冲区视图、
    // 几何缓冲区和上传缓冲区、命令分配器、命令列表以及围栏。
    // 这些资源由 initialize() 创建，并在 shutdown() 或 resize() 中释放。
    struct Dx12Renderer {
      public:
        Dx12Renderer(AllocatorView allocator)
            : textures(allocator), pendingUploads(allocator), freeDescriptors(allocator) {}
        auto initialize(ID3D12Device *device, ID3D12CommandQueue *queue, IDXGISwapChain3 *swapChain) noexcept {
            REZERO_ASSERT(device);
            REZERO_ASSERT(queue);
            REZERO_ASSERT(swapChain);

            Device = static_cast<ID3D12Device *>(device);
            Queue = static_cast<ID3D12CommandQueue *>(queue);
            SwapChain = static_cast<IDXGISwapChain3 *>(swapChain);

            if (!createCommandObjects() || !createFence() || !createPipeline()) {
                shutdown();
                return;
            }
            return;
        }
        auto shutdown() noexcept -> void {
            // The GPU may still be reading whatever is released below; the frame that
            // was submitted last always finishes first, because renderDrawData waits
            // for it, but a host may shut down without ever having rendered.
            waitForGpu();
            releasePipeline();
            releaseRenderTargets();
            releaseCommandObjects();
            releaseFence();
            // Not owned by the backend: just forget the host's references.
            Device = nullptr;
            Queue = nullptr;
            SwapChain = nullptr;
            backbufferWidth = 0;
            backbufferHeight = 0;
        }
        // 释放渲染目标视图（RTV），以便宿主调整交换链缓冲区的大小。
        // 必须在调用 IDXGISwapChain::ResizeBuffers 之前执行此操作。
        // 新的后台缓冲区创建完成后，后端会在下一次调用 renderDrawData 时
        // 根据新的后台缓冲区重新创建渲染目标视图。
        //
        // 交换链归宿主所有，因此宿主也必须自行释放其持有的后台缓冲区引用，
        // 并调用 ResizeBuffers。
        //
        // 后端不会保留后台缓冲区本身的引用——每一帧在通过资源状态转换屏障
        // 将缓冲区转换为 RENDER_TARGET 状态时，都会重新从交换链获取缓冲区。
        // 不过，后端会在这里停止使用旧的缓冲区：等待上一帧提交的 GPU 工作完成，
        // 并回收用于记录该帧命令的命令分配器，确保后端提交的工作不再持有
        // 任何旧缓冲区的引用。
        void resize(std::uint32_t, std::uint32_t) noexcept {
            waitForGpu();
            releaseFrameReferences();
            releaseRenderTargets();
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
        ///
        /// The pixels are not uploaded here: they only travel to the GPU once the
        /// frame's single upload buffer has been sized for everything it carries.
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

            const std::uint32_t descriptor = allocateDescriptor();
            if (descriptor == InvalidDescriptor) {
                return false;
            }

            ID3D12Resource *texture = nullptr;
            // A new texture is born in COPY_DEST, which is where the frame's upload
            // leaves it before the barrier to PIXEL_SHADER_RESOURCE.
            if (!createTextureResource(width, height, D3D12_RESOURCE_STATE_COPY_DEST, texture)) {
                releaseDescriptor(descriptor);
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            Device->CreateShaderResourceView(texture, &view, descriptorCpu(descriptor));

            record->texture = texture;
            record->width = width;
            record->height = height;
            record->descriptor = descriptor;
            record->state = D3D12_RESOURCE_STATE_COPY_DEST;

            // A create without pixels still has to reach PIXEL_SHADER_RESOURCE before
            // anything samples it, so the upload slot is queued even then.
            return queueUpload(handle, rgba);
        }

        /// Re-uploads the pixels of an existing texture. The texture must have been
        /// created with the same dimensions. The copy reaches the GPU with the next
        /// frame, so `rgba` has to stay valid until then - which is the same contract
        /// the UI's resource commands already have.
        bool updateTexture(TextureId handle, const std::uint8_t *rgba) noexcept {
            TextureRecord *record = textureFor(handle);
            if (record == nullptr || record->texture == nullptr || Device == nullptr || rgba == nullptr) {
                return false;
            }
            if (record->width == 0 || record->height == 0) {
                return false;
            }
            return queueUpload(handle, rgba);
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
            if (Device == nullptr || Queue == nullptr || commandList == nullptr || !data.valid) {
                return;
            }
            if (rtvHeap == nullptr && !createRenderTargets()) {
                return;
            }

            // The wait that ends the previous frame already left the GPU idle; this is
            // cheap insurance for a host that submitted work of its own in between.
            waitForGpu();
            if (FAILED(commandAllocator->Reset()) || FAILED(commandList->Reset(commandAllocator, nullptr))) {
                return;
            }

            // Textures are published before the geometry that samples them. The queue
            // only grows when the UI creates or destroys a texture, so replaying it
            // every frame is cheap and also restores every handle after a resize.
            applyResourceCommands(data.resourceCommands);

            const std::uint32_t backgroundVertices = data.background != nullptr ? data.background->vertexCount() : 0;
            const std::uint32_t backgroundIndices = data.background != nullptr ? data.background->indexCount() : 0;
            const std::uint32_t foregroundVertices = data.foreground != nullptr ? data.foreground->vertexCount() : 0;
            const std::uint32_t foregroundIndices = data.foreground != nullptr ? data.foreground->indexCount() : 0;
            const std::uint32_t vertexCount =
                backgroundVertices + foregroundVertices + (data.overlay != nullptr ? data.overlay->vertexCount() : 0);
            const std::uint32_t indexCount =
                backgroundIndices + foregroundIndices + (data.overlay != nullptr ? data.overlay->indexCount() : 0);
            const std::uint64_t vertexBytes = static_cast<std::uint64_t>(vertexCount) * sizeof(DrawVertex);
            const std::uint64_t indexBytes = static_cast<std::uint64_t>(indexCount) * sizeof(DrawIndex);

            // One upload buffer carries everything the frame needs on the CPU side: the
            // queued texture uploads first, then the vertices and the indices. Laying
            // the offsets out before the buffer is sized is what keeps a grown buffer
            // from invalidating a copy that was already recorded against the old one.
            std::uint64_t vertexOffset = 0;
            std::uint64_t indexOffset = 0;
            const std::uint64_t uploadBytes = layOutUploads(vertexBytes, indexBytes, vertexOffset, indexOffset);

            if (!ensureBuffers(vertexCount, indexCount) || !ensureUploadBuffer(uploadBytes)) {
                // Nothing was recorded yet, but Reset only accepts a closed list.
                commandList->Close();
                return;
            }

            fillUploadBuffer(data, backgroundVertices, backgroundIndices, foregroundVertices, foregroundIndices,
                             vertexOffset, indexOffset);
            recordGeometryUpload(vertexBytes, indexBytes, vertexOffset, indexOffset);
            recordTextureUploads();

            setProjection(static_cast<float>(data.displaySize.x), static_cast<float>(data.displaySize.y));

            const std::uint32_t backBufferIndex = SwapChain->GetCurrentBackBufferIndex();
            ID3D12Resource *backBuffer = nullptr;
            if (backBufferIndex >= rtvCount ||
                FAILED(SwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)))) {
                commandList->Close();
                return;
            }
            nameObject(backBuffer, L"ReZeroGui swap chain back buffer");

            // The swap chain hands a buffer out in PRESENT; it has to be a render
            // target before it can be cleared or drawn into.
            recordTransition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            bindPipeline();
            commandList->ClearRenderTargetView(renderTargetView(backBufferIndex), ClearColor, 0, nullptr);

            if (data.background != nullptr) {
                renderList(*data.background, 0, 0);
            }
            if (data.foreground != nullptr) {
                renderList(*data.foreground, backgroundVertices, backgroundIndices);
            }
            if (data.overlay != nullptr) {
                renderList(*data.overlay, backgroundVertices + foregroundVertices,
                           backgroundIndices + foregroundIndices);
            }

            // The host presents straight after this call, so the frame hands the buffer
            // back in the state Present expects.
            recordTransition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            backBuffer->Release();

            if (FAILED(commandList->Close())) {
                return;
            }
            ID3D12CommandList *lists[] = {commandList};
            Queue->ExecuteCommandLists(1, lists);

            // Deliberate: waiting for the frame here trades throughput for a host that
            // can Present immediately and read the back buffer back on the CPU. It is
            // also what makes one frame in flight enough - every buffer and texture the
            // next frame overwrites is provably unused by the GPU by the time it does.
            submitAndWait();

            // Everything queued has been copied; the next frame queues what it needs.
            pendingUploads.clear();
        }

        /// Colour the render target is cleared with before the UI is drawn.
        ///
        /// Unlike Direct3D 11 the host cannot clear the back buffer itself: the only
        /// command list a frame is recorded into belongs to this backend, and the back
        /// buffer is in RENDER_TARGET state only while that list runs. The renderer
        /// therefore clears, and this is the colour it uses. A host that wants a
        /// different background draws it as its own geometry after the clear.
        static constexpr float ClearColor[4] = {0.09f, 0.09f, 0.12f, 1.0f};

      private:
        /// Slot of the shader visible heap that is not a slot.
        static constexpr std::uint32_t InvalidDescriptor = 0xFFFFFFFFu;

        struct TextureRecord {
            ID3D12Resource *texture{nullptr};
            std::uint32_t width{0};
            std::uint32_t height{0};
            /// Where the resource is right now, so an update knows whether a transition
            /// to COPY_DEST is still needed. Direct3D 12 fails a frame whose barriers do
            /// not name the state a resource is really in, so this is tracked per record
            /// rather than assumed.
            D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
            /// Slot in the shader visible heap. It belongs to the record, so a texture
            /// the UI keeps alive keeps its descriptor instead of taking a new one every
            /// frame.
            std::uint32_t descriptor{InvalidDescriptor};
        };

        /// One texture upload queued for this frame: which handle, where it lands in the
        /// frame's upload buffer, and which bytes feed it.
        struct PendingUpload {
            TextureId handle = InvalidTexture;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT placement{};
            std::uint64_t size{0};
            const std::uint8_t *pixels = nullptr; // RGBA8, borrowed
        };

        struct VertexConstants {
            float projection[16];
        };

        /// Direct3D 12 requires the placed footprint of a texture copy to start on a 512
        /// byte boundary. Keeping the geometry on the same boundary costs a few bytes
        /// and means only one alignment rule has to hold for the whole buffer.
        static constexpr std::uint64_t PlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

        /// Slots the shader visible heap starts with; it doubles whenever a frame needs
        /// more than it has.
        static constexpr std::uint32_t InitialDescriptorCapacity = 64;

        static constexpr std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        /// Names a device object. Nothing needs the name to draw, but the debug layer
        /// quotes it when it complains, which is the difference between "a resource was
        /// released too early" and knowing which one.
        static void nameObject(ID3D12Object *object, const wchar_t *name) noexcept {
            if (object != nullptr) {
                object->SetName(name);
            }
        }

        static void defaultHeapProperties(D3D12_HEAP_PROPERTIES &properties) noexcept {
            properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            properties.CreationNodeMask = 1;
            properties.VisibleNodeMask = 1;
        }

        /// Describes a UI texture: RGBA8 with red in byte 0, which is what
        /// DXGI_FORMAT_R8G8B8A8_UNORM already means, so nothing is swizzled.
        static auto textureDescription(std::uint32_t width, std::uint32_t height) noexcept -> D3D12_RESOURCE_DESC {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = width;
            description.Height = height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            return description;
        }

        /// Releases the resource of one slot, returns its descriptor to the heap and
        /// leaves the slot empty. Safe to call on an already empty slot.
        void releaseRecord(TextureRecord &record) noexcept {
            if (record.texture != nullptr) {
                record.texture->Release();
                record.texture = nullptr;
            }
            if (record.descriptor != InvalidDescriptor) {
                releaseDescriptor(record.descriptor);
                record.descriptor = InvalidDescriptor;
            }
            record.width = 0;
            record.height = 0;
            record.state = D3D12_RESOURCE_STATE_COMMON;
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

        // -- descriptors ---------------------------------------------------------

        auto descriptorCpu(std::uint32_t slot) const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(slot) * srvDescriptorSize;
            return handle;
        }

        auto descriptorGpu(std::uint32_t slot) const noexcept -> D3D12_GPU_DESCRIPTOR_HANDLE {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(slot) * srvDescriptorSize;
            return handle;
        }

        /// Hands out one slot of the shader visible heap, reusing a slot a destroyed
        /// texture gave back before growing the heap.
        auto allocateDescriptor() noexcept -> std::uint32_t {
            if (!freeDescriptors.empty()) {
                const std::uint32_t slot = freeDescriptors.back();
                freeDescriptors.pop_back();
                return slot;
            }
            if (nextDescriptor + 1 > descriptorCapacity && !growDescriptorHeap()) {
                return InvalidDescriptor;
            }
            const std::uint32_t slot = nextDescriptor;
            nextDescriptor += 1;
            return slot;
        }

        void releaseDescriptor(std::uint32_t slot) noexcept {
            // A full table only means a leaked slot, which is not worth failing over.
            freeDescriptors.push_back(slot);
        }

        /// Doubles the shader visible heap and copies the live descriptors into the new
        /// one, so every slot keeps its index and no texture has to be re-described.
        /// The old heap can be released without waiting because a frame's work has
        /// finished by the time the next frame gets here.
        auto growDescriptorHeap() noexcept -> bool {
            const std::uint32_t capacity = descriptorCapacity == 0 ? InitialDescriptorCapacity : descriptorCapacity * 2;

            D3D12_DESCRIPTOR_HEAP_DESC description{};
            description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            description.NumDescriptors = capacity;
            description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            ID3D12DescriptorHeap *heap = nullptr;
            if (FAILED(Device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&heap)))) {
                return false;
            }
            if (srvHeap != nullptr) {
                if (nextDescriptor > 0) {
                    Device->CopyDescriptorsSimple(nextDescriptor, heap->GetCPUDescriptorHandleForHeapStart(),
                                                  srvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                srvHeap->Release();
            }
            srvHeap = heap;
            nameObject(srvHeap, L"ReZeroGui SRV heap");
            srvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            descriptorCapacity = capacity;
            return true;
        }

        // -- resources -----------------------------------------------------------

        auto createTextureResource(std::uint32_t width, std::uint32_t height, D3D12_RESOURCE_STATES state,
                                   ID3D12Resource *&texture) noexcept -> bool {
            const D3D12_RESOURCE_DESC description = textureDescription(width, height);
            D3D12_HEAP_PROPERTIES properties{};
            defaultHeapProperties(properties);
            const HRESULT created = Device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                                    state, nullptr, IID_PPV_ARGS(&texture));
            nameObject(texture, L"ReZeroGui UI texture");
            return SUCCEEDED(created);
        }

        auto createBufferResource(std::uint64_t size, ID3D12Resource *&buffer) noexcept -> bool {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            // Buffer sizes are rounded up so that a growth step cannot hand the device a
            // size it dislikes.
            description.Width = alignUp(size, 256);
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES properties{};
            defaultHeapProperties(properties);
            // Direct3D 12 ignores the creation state of a buffer and hands it out in
            // COMMON, so every buffer tracking member starts from COMMON too.
            const HRESULT created =
                Device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer));
            nameObject(buffer, L"ReZeroGui geometry buffer");
            return SUCCEEDED(created);
        }

        /// Queues one upload of `pixels` into the texture behind `handle`. The footprint
        /// is worked out now so the frame's upload buffer can be sized before anything
        /// is written into it; the row pitch is whatever the device wants for the
        /// format, which is not simply width * 4.
        auto queueUpload(TextureId handle, const std::uint8_t *pixels) noexcept -> bool {
            TextureRecord *record = textureFor(handle);
            if (record == nullptr || record->texture == nullptr) {
                return false;
            }

            PendingUpload upload;
            upload.handle = handle;
            upload.pixels = pixels;

            const D3D12_RESOURCE_DESC description = textureDescription(record->width, record->height);
            UINT rows = 0;
            UINT64 rowSize = 0;
            UINT64 total = 0;
            Device->GetCopyableFootprints(&description, 0, 1, 0, &upload.placement, &rows, &rowSize, &total);
            upload.size = total;
            return pendingUploads.push_back(upload);
        }

        /// Builds the 1x1 opaque white texture that untextured primitives sample, so the
        /// pixel shader can stay a single multiply for both cases.
        auto createWhiteTexture() noexcept -> bool {
            static const std::uint8_t White[4] = {255, 255, 255, 255};

            whiteDescriptor = allocateDescriptor();
            if (whiteDescriptor == InvalidDescriptor) {
                return false;
            }

            // A texture may not live in an upload heap, so the white texel is an
            // ordinary device local texture. It is filled once, here, through the same
            // staging buffer the frames use, which is what lets the first frame treat it
            // like any other texture instead of special casing it.
            if (!createTextureResource(1, 1, D3D12_RESOURCE_STATE_COPY_DEST, whiteTexture)) {
                releaseDescriptor(whiteDescriptor);
                whiteDescriptor = InvalidDescriptor;
                return false;
            }
            nameObject(whiteTexture, L"ReZeroGui white texture");

            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            Device->CreateShaderResourceView(whiteTexture, &view, descriptorCpu(whiteDescriptor));

            return uploadWhiteTexture(White);
        }

        /// Stages the white texel in the frame's upload buffer, copies it into the white
        /// texture and waits for that copy. It is the only upload the backend records
        /// outside a frame, and it happens before the host can ask for one.
        auto uploadWhiteTexture(const std::uint8_t *white) noexcept -> bool {
            if (!ensureUploadBuffer(4)) {
                return false;
            }
            auto *const base = static_cast<std::uint8_t *>(uploadPointer);
            if (base == nullptr) {
                return false;
            }

            const D3D12_RESOURCE_DESC description = textureDescription(1, 1);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT placement{};
            UINT rows = 0;
            UINT64 rowSize = 0;
            UINT64 total = 0;
            Device->GetCopyableFootprints(&description, 0, 1, 0, &placement, &rows, &rowSize, &total);
            std::memcpy(base + placement.Offset, white, 4);

            if (FAILED(commandAllocator->Reset()) || FAILED(commandList->Reset(commandAllocator, nullptr))) {
                return false;
            }

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = uploadBuffer;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = placement;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = whiteTexture;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = 0;

            commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            recordTransition(whiteTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            if (FAILED(commandList->Close())) {
                return false;
            }
            ID3D12CommandList *lists[] = {commandList};
            Queue->ExecuteCommandLists(1, lists);
            submitAndWait();
            return true;
        }

        // -- render targets ------------------------------------------------------

        /// Creates one render target view per swap chain buffer, so whichever buffer
        /// GetCurrentBackBufferIndex names is ready. The backend keeps no reference to
        /// the buffers themselves: the swap chain owns them, and each frame asks for the
        /// current one again when it needs to build a barrier.
        auto createRenderTargets() noexcept -> bool {
            if (Device == nullptr || SwapChain == nullptr) {
                return false;
            }

            DXGI_SWAP_CHAIN_DESC1 description{};
            if (FAILED(SwapChain->GetDesc1(&description)) || description.BufferCount == 0) {
                return false;
            }
            backbufferWidth = description.Width;
            backbufferHeight = description.Height;

            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
            heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDescription.NumDescriptors = description.BufferCount;
            if (FAILED(Device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&rtvHeap)))) {
                return false;
            }
            nameObject(rtvHeap, L"ReZeroGui RTV heap");
            rtvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            rtvCount = description.BufferCount;

            for (std::uint32_t index = 0; index < rtvCount; ++index) {
                ID3D12Resource *backbuffer = nullptr;
                if (FAILED(SwapChain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
                    return false;
                }
                Device->CreateRenderTargetView(backbuffer, nullptr, renderTargetView(index));
                backbuffer->Release();
            }
            return true;
        }

        auto renderTargetView(std::uint32_t index) const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * rtvDescriptorSize;
            return handle;
        }

        auto releaseRenderTargets() noexcept -> void {
            if (rtvHeap != nullptr) {
                rtvHeap->Release();
                rtvHeap = nullptr;
            }
            rtvCount = 0;
        }

        // -- synchronisation -----------------------------------------------------

        auto createCommandObjects() noexcept -> bool {
            if (FAILED(
                    Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)))) {
                return false;
            }
            if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr,
                                                 IID_PPV_ARGS(&commandList)))) {
                return false;
            }
            // CreateCommandList hands the list back open, and Reset only accepts a
            // closed one, so the first frame would otherwise record into an open list.
            nameObject(commandAllocator, L"ReZeroGui command allocator");
            nameObject(commandList, L"ReZeroGui command list");
            return SUCCEEDED(commandList->Close());
        }

        auto releaseCommandObjects() noexcept -> void {
            auto releaseAndClear = [](auto &comPointer) noexcept {
                if (comPointer != nullptr) {
                    comPointer->Release();
                    comPointer = nullptr;
                }
            };
            releaseAndClear(commandList);
            releaseAndClear(commandAllocator);
        }

        auto createFence() noexcept -> bool {
            if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                return false;
            }
            nameObject(fence, L"ReZeroGui frame fence");
            fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return fenceEvent != nullptr;
        }

        auto releaseFence() noexcept -> void {
            if (fence != nullptr) {
                fence->Release();
                fence = nullptr;
            }
            if (fenceEvent != nullptr) {
                ::CloseHandle(fenceEvent);
                fenceEvent = nullptr;
            }
            fenceValue = 0;
        }

        /// Blocks until the GPU has passed the last submitted fence value. The value a
        /// frame waits on is set by submitAndWait, so this is a no-op between frames.
        auto waitForGpu() noexcept -> void {
            if (fence == nullptr || fenceEvent == nullptr || fenceValue == 0) {
                return;
            }
            if (fence->GetCompletedValue() >= fenceValue) {
                return;
            }
            if (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent))) {
                return;
            }
            ::WaitForSingleObject(fenceEvent, INFINITE);
        }

        /// Signals the fence for everything submitted so far and blocks until the GPU
        /// has passed it.
        auto submitAndWait() noexcept -> void {
            if (fence == nullptr || Queue == nullptr || fenceEvent == nullptr) {
                return;
            }
            const std::uint64_t value = fenceValue + 1;
            if (FAILED(Queue->Signal(fence, value))) {
                return;
            }
            fenceValue = value;
            if (fence->GetCompletedValue() >= value) {
                return;
            }
            if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) {
                return;
            }
            ::WaitForSingleObject(fenceEvent, INFINITE);
        }

        /// Lets the runtime drop everything the last frame's command list still holds and
        /// returns the list closed, the way renderDrawData expects to find it. Only safe
        /// once the GPU has passed the frame: renderDrawData waits, so this normally is.
        auto releaseFrameReferences() noexcept -> bool {
            if (commandAllocator == nullptr || commandList == nullptr) {
                return false;
            }
            if (FAILED(commandAllocator->Reset()) || FAILED(commandList->Reset(commandAllocator, nullptr))) {
                return false;
            }
            return SUCCEEDED(commandList->Close());
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

            if (FAILED(D3DCompile(PixelSource, std::strlen(PixelSource), nullptr, nullptr, nullptr, "pixelMain",
                                  "ps_5_0", flags, 0, &pixelBlob, &errorBlob))) {
                releaseBlob(errorBlob);
                return false;
            }
            releaseBlob(errorBlob);
            return true;
        }

        /// A minimal root signature: the projection matrix as 16 root constants and one
        /// descriptor table holding the single sampled texture.
        ///
        /// Root constants are pushed by the command list straight out of CPU memory, so
        /// the matrix needs neither a constant buffer nor an upload. The sampler is
        /// static - part of the root signature rather than a heap - because the UI
        /// always samples the same way.
        auto createRootSignature() noexcept -> bool {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = 0;
            range.RegisterSpace = 0;
            range.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants.ShaderRegister = 0;
            parameters[0].Constants.RegisterSpace = 0;
            parameters[0].Constants.Num32BitValues = 16;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable.NumDescriptorRanges = 1;
            parameters[1].DescriptorTable.pDescriptorRanges = &range;
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MipLODBias = 0.0f;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            sampler.MinLOD = 0.0f;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = 0;
            sampler.RegisterSpace = 0;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
            description.Version = D3D_ROOT_SIGNATURE_VERSION_1;
            description.Desc_1_0.NumParameters = 2;
            description.Desc_1_0.pParameters = parameters;
            description.Desc_1_0.NumStaticSamplers = 1;
            description.Desc_1_0.pStaticSamplers = &sampler;
            description.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ID3DBlob *signature = nullptr;
            ID3DBlob *errorBlob = nullptr;
            const HRESULT serialized = D3D12SerializeVersionedRootSignature(&description, &signature, &errorBlob);
            releaseBlob(errorBlob);
            if (FAILED(serialized)) {
                return false;
            }

            const HRESULT created = Device->CreateRootSignature(
                0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
            nameObject(rootSignature, L"ReZeroGui root signature");
            releaseBlob(signature);
            return SUCCEEDED(created);
        }

        /// Everything the Direct3D 11 backend spreads over separate state objects is part
        /// of the pipeline state here: blend, rasterizer, depth-stencil, the input layout
        /// and the render target format.
        auto createPipelineState() noexcept -> bool {
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(DrawVertex, position),
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(DrawVertex, uv),
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(DrawVertex, pixel),
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            static_assert(offsetof(DrawVertex, position) == 0);
            static_assert(offsetof(DrawVertex, uv) == 8);
            static_assert(offsetof(DrawVertex, pixel) == 16);
            static_assert(sizeof(DrawVertex) == 20);

            DXGI_SWAP_CHAIN_DESC1 swapDescription{};
            if (FAILED(SwapChain->GetDesc1(&swapDescription))) {
                return false;
            }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = rootSignature;
            description.VS.pShaderBytecode = vertexBlob->GetBufferPointer();
            description.VS.BytecodeLength = vertexBlob->GetBufferSize();
            description.PS.pShaderBytecode = pixelBlob->GetBufferPointer();
            description.PS.BytecodeLength = pixelBlob->GetBufferSize();

            description.BlendState.AlphaToCoverageEnable = FALSE;
            description.BlendState.IndependentBlendEnable = FALSE;
            description.BlendState.RenderTarget[0].BlendEnable = TRUE;
            description.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            description.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            description.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            description.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            description.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            description.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            description.SampleMask = UINT_MAX;

            description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            description.RasterizerState.DepthClipEnable = TRUE;
            // Scissoring is always on in Direct3D 12: a rect per command is what the
            // rasterizer state of the Direct3D 11 backend asks for with ScissorEnable.

            // No depth buffer: the UI is drawn in submission order and blended.
            description.DepthStencilState.DepthEnable = FALSE;
            description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            description.DepthStencilState.StencilEnable = FALSE;
            description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
            description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
            description.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
            description.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            description.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
            description.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;

            description.InputLayout.pInputElementDescs = layout;
            description.InputLayout.NumElements = static_cast<UINT>(std::size(layout));
            description.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = swapDescription.Format;
            description.DSVFormat = DXGI_FORMAT_UNKNOWN;
            description.SampleDesc.Count = 1;
            description.NodeMask = 0;
            description.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

            const HRESULT created = Device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipelineState));
            nameObject(pipelineState, L"ReZeroGui pipeline state");
            // The pixel shader lives on inside the state object; only the vertex blob is
            // kept, the way the Direct3D 11 backend keeps it for its input layout.
            releaseBlob(pixelBlob);
            return SUCCEEDED(created);
        }

        auto createPipeline() noexcept -> bool {
            if (!compileShaders()) {
                return false;
            }
            if (!createRootSignature()) {
                return false;
            }
            if (!createPipelineState()) {
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
            freeDescriptors.clear();
            nextDescriptor = 0;

            if (whiteTexture != nullptr) {
                whiteTexture->Release();
                whiteTexture = nullptr;
            }
            whiteDescriptor = InvalidDescriptor;

            releaseGeometryBuffers();
            releaseUploadBuffer();

            // Release each COM pointer exactly once and forget it; any of them
            // may still be null when the pipeline was only partially created.
            auto releaseAndClear = [](auto &comPointer) noexcept {
                if (comPointer != nullptr) {
                    comPointer->Release();
                    comPointer = nullptr;
                }
            };
            releaseAndClear(pipelineState);
            releaseAndClear(rootSignature);
            releaseAndClear(pixelBlob);
            releaseAndClear(vertexBlob);
            if (srvHeap != nullptr) {
                srvHeap->Release();
                srvHeap = nullptr;
            }
            descriptorCapacity = 0;
            vertexCapacity = 0;
            indexCapacity = 0;
        }

        void releaseGeometryBuffers() noexcept {
            if (vertexBuffer != nullptr) {
                vertexBuffer->Release();
                vertexBuffer = nullptr;
            }
            if (indexBuffer != nullptr) {
                indexBuffer->Release();
                indexBuffer = nullptr;
            }
            vertexBufferState = D3D12_RESOURCE_STATE_COMMON;
            indexBufferState = D3D12_RESOURCE_STATE_COMMON;
        }

        void releaseUploadBuffer() noexcept {
            if (uploadBuffer != nullptr) {
                uploadBuffer->Unmap(0, nullptr);
                uploadBuffer->Release();
                uploadBuffer = nullptr;
            }
            uploadPointer = nullptr;
            uploadCapacity = 0;
        }

        /// Grows the geometry buffers the frame's whole draw list is copied into. Both
        /// are device local: the copy out of the upload buffer is what hands the GPU
        /// memory it can read at full speed, which a mapped upload heap cannot.
        auto ensureBuffers(std::uint32_t vertexCount, std::uint32_t indexCount) noexcept -> bool {
            if (vertexCount > vertexCapacity) {
                if (vertexBuffer != nullptr) {
                    vertexBuffer->Release();
                    vertexBuffer = nullptr;
                }
                const std::uint32_t capacity = vertexCount + vertexCount / 2 + 5000;
                if (!createBufferResource(static_cast<std::uint64_t>(capacity) * sizeof(DrawVertex), vertexBuffer)) {
                    return false;
                }
                vertexCapacity = capacity;
                vertexBufferState = D3D12_RESOURCE_STATE_COMMON;
            }

            if (indexCount > indexCapacity) {
                if (indexBuffer != nullptr) {
                    indexBuffer->Release();
                    indexBuffer = nullptr;
                }
                const std::uint32_t capacity = indexCount + indexCount / 2 + 10000;
                if (!createBufferResource(static_cast<std::uint64_t>(capacity) * sizeof(DrawIndex), indexBuffer)) {
                    return false;
                }
                indexCapacity = capacity;
                indexBufferState = D3D12_RESOURCE_STATE_COMMON;
            }
            return true;
        }

        /// Makes sure the one upload buffer a frame stages its CPU side data in can hold
        /// `bytes`. It stays mapped from creation to release, so a frame writes through
        /// a plain pointer instead of paying for a Map/Unmap pair every frame.
        auto ensureUploadBuffer(std::uint64_t bytes) noexcept -> bool {
            if (uploadBuffer != nullptr && bytes <= uploadCapacity) {
                return true;
            }
            releaseUploadBuffer();

            const std::uint64_t capacity = bytes + bytes / 2 + 65536;

            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = alignUp(capacity, 256);
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_UPLOAD;
            properties.CreationNodeMask = 1;
            properties.VisibleNodeMask = 1;

            if (FAILED(Device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&uploadBuffer)))) {
                return false;
            }
            nameObject(uploadBuffer, L"ReZeroGui upload buffer");

            // The CPU only ever writes this memory; saying so lets the driver skip the
            // cache work a read range would ask for.
            const D3D12_RANGE readRange{0, 0};
            if (FAILED(uploadBuffer->Map(0, &readRange, &uploadPointer))) {
                releaseUploadBuffer();
                return false;
            }
            uploadCapacity = capacity;
            return true;
        }

        /// Assigns every queued upload its slice of the frame's upload buffer and returns
        /// how many bytes the buffer has to hold in total.
        auto layOutUploads(std::uint64_t vertexBytes, std::uint64_t indexBytes, std::uint64_t &vertexOffset,
                           std::uint64_t &indexOffset) noexcept -> std::uint64_t {
            std::uint64_t cursor = 0;
            for (PendingUpload &upload : pendingUploads) {
                cursor = alignUp(cursor, PlacementAlignment);
                upload.placement.Offset = cursor;
                cursor += upload.size;
            }
            cursor = alignUp(cursor, PlacementAlignment);
            vertexOffset = cursor;
            cursor += vertexBytes;
            cursor = alignUp(cursor, PlacementAlignment);
            indexOffset = cursor;
            cursor += indexBytes;
            return cursor;
        }

        /// Copies the frame's CPU side data into the mapped upload buffer: the queued
        /// texture pixels first, then the geometry of both draw lists, background before
        /// foreground so the draw order's index and vertex bases line up.
        void fillUploadBuffer(const DrawData &data, std::uint32_t backgroundVertices, std::uint32_t backgroundIndices,
                              std::uint32_t foregroundVertices, std::uint32_t foregroundIndices,
                              std::uint64_t vertexOffset, std::uint64_t indexOffset) noexcept {
            auto *const base = static_cast<std::uint8_t *>(uploadPointer);
            if (base == nullptr) {
                return;
            }

            for (const PendingUpload &upload : pendingUploads) {
                if (upload.pixels == nullptr) {
                    continue;
                }
                const TextureRecord *record = textureFor(upload.handle);
                if (record == nullptr) {
                    continue;
                }
                const std::uint32_t rowBytes = record->width * 4;
                const std::uint32_t rowPitch = upload.placement.Footprint.RowPitch;
                std::uint8_t *destination = base + upload.placement.Offset;
                for (std::uint32_t row = 0; row < record->height; ++row) {
                    std::memcpy(destination + static_cast<std::size_t>(row) * rowPitch,
                                upload.pixels + static_cast<std::size_t>(row) * rowBytes, rowBytes);
                }
            }

            if (data.background != nullptr && backgroundVertices != 0) {
                std::memcpy(base + vertexOffset, data.background->currentVertices().data(),
                            static_cast<std::size_t>(backgroundVertices) * sizeof(DrawVertex));
                std::memcpy(base + indexOffset, data.background->currentIndices().data(),
                            static_cast<std::size_t>(backgroundIndices) * sizeof(DrawIndex));
            }
            if (data.foreground != nullptr && foregroundVertices != 0) {
                const std::uint64_t foregroundVertexOffset =
                    vertexOffset + static_cast<std::uint64_t>(backgroundVertices) * sizeof(DrawVertex);
                const std::uint64_t foregroundIndexOffset =
                    indexOffset + static_cast<std::uint64_t>(backgroundIndices) * sizeof(DrawIndex);
                std::memcpy(base + foregroundVertexOffset, data.foreground->currentVertices().data(),
                            static_cast<std::size_t>(foregroundVertices) * sizeof(DrawVertex));
                std::memcpy(base + foregroundIndexOffset, data.foreground->currentIndices().data(),
                            static_cast<std::size_t>(foregroundIndices) * sizeof(DrawIndex));
            }
            if (data.overlay != nullptr && data.overlay->vertexCount() != 0) {
                const std::uint64_t overlayVertexOffset =
                    vertexOffset +
                    static_cast<std::uint64_t>(backgroundVertices + foregroundVertices) * sizeof(DrawVertex);
                const std::uint64_t overlayIndexOffset =
                    indexOffset + static_cast<std::uint64_t>(backgroundIndices + foregroundIndices) * sizeof(DrawIndex);
                std::memcpy(base + overlayVertexOffset, data.overlay->currentVertices().data(),
                            static_cast<std::size_t>(data.overlay->vertexCount()) * sizeof(DrawVertex));
                std::memcpy(base + overlayIndexOffset, data.overlay->currentIndices().data(),
                            static_cast<std::size_t>(data.overlay->indexCount()) * sizeof(DrawIndex));
            }
        }

        /// Records one transition. Direct3D 12 fails a frame whose barriers do not name
        /// the state a resource is really in, so every transition here is spelled with
        /// the state the tracking member holds.
        void recordTransition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after) noexcept {
            if (resource == nullptr || before == after) {
                return;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            commandList->ResourceBarrier(1, &barrier);
        }

        /// Moves the staged geometry into the device local buffers. Both are in a read
        /// state from the previous frame, so each one passes through COPY_DEST.
        void recordGeometryUpload(std::uint64_t vertexBytes, std::uint64_t indexBytes, std::uint64_t vertexOffset,
                                  std::uint64_t indexOffset) noexcept {
            if (vertexBytes != 0) {
                recordTransition(vertexBuffer, vertexBufferState, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->CopyBufferRegion(vertexBuffer, 0, uploadBuffer, vertexOffset, vertexBytes);
                recordTransition(vertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                vertexBufferState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            }
            if (indexBytes != 0) {
                recordTransition(indexBuffer, indexBufferState, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->CopyBufferRegion(indexBuffer, 0, uploadBuffer, indexOffset, indexBytes);
                recordTransition(indexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                indexBufferState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
            }
        }

        /// Records the texture copies queued for this frame. They run before any draw is
        /// recorded, so a texture the UI just created or updated is read in the state the
        /// copy leaves it in - no separate submission or fence wait is needed.
        void recordTextureUploads() noexcept {
            for (const PendingUpload &upload : pendingUploads) {
                TextureRecord *record = textureFor(upload.handle);
                if (record == nullptr || record->texture == nullptr) {
                    continue; // destroyed again before the frame was recorded
                }

                // A texture that was created this frame is already in COPY_DEST.
                if (record->state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    recordTransition(record->texture, record->state, D3D12_RESOURCE_STATE_COPY_DEST);
                    record->state = D3D12_RESOURCE_STATE_COPY_DEST;
                }

                if (upload.pixels != nullptr) {
                    D3D12_TEXTURE_COPY_LOCATION source{};
                    source.pResource = uploadBuffer;
                    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    source.PlacedFootprint = upload.placement;

                    D3D12_TEXTURE_COPY_LOCATION destination{};
                    destination.pResource = record->texture;
                    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    destination.SubresourceIndex = 0;

                    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                }

                recordTransition(record->texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                record->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
        }

        /// Fills the root constants with the same off centre orthographic projection the
        /// Direct3D 11 backend builds: pixel coordinates with the origin at the top left.
        /// Nothing is uploaded - the command list pushes these 64 bytes itself.
        auto setProjection(float displayWidth, float displayHeight) noexcept -> void {
            const float left = 0.0f;
            const float right = displayWidth;
            const float top = 0.0f;
            const float bottom = displayHeight;

            constants = VertexConstants{};
            constants.projection[0] = 2.0f / (right - left);
            constants.projection[5] = 2.0f / (top - bottom);
            constants.projection[10] = 0.5f;
            constants.projection[12] = (right + left) / (left - right);
            constants.projection[13] = (top + bottom) / (bottom - top);
            constants.projection[14] = 0.5f;
            constants.projection[15] = 1.0f;
        }

        /// Records the draws of one list. `vertexBase` and `indexBase` are where the list
        /// sits in the frame's shared buffers: the background is staged at zero and the
        /// foreground after it, so both survive into the same command list.
        auto renderList(const DrawList &list, std::uint32_t vertexBase, std::uint32_t indexBase) noexcept -> void {
            if (list.isEmpty()) {
                return;
            }

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

                const D3D12_RECT scissor{static_cast<LONG>(clip.x), static_cast<LONG>(clip.y),
                                         static_cast<LONG>(clip.maxX()), static_cast<LONG>(clip.maxY())};
                if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
                    continue; // the clip rounds away to nothing in whole pixels
                }
                commandList->RSSetScissorRects(1, &scissor);

                // An unknown, invalid or destroyed handle samples the white texel, so a
                // widget that lost its texture still draws as if it sampled white.
                const TextureRecord *record = textureFor(command.textureId);
                std::uint32_t slot = whiteDescriptor;
                if (record != nullptr && record->descriptor != InvalidDescriptor) {
                    slot = record->descriptor;
                }
                commandList->SetGraphicsRootDescriptorTable(1, descriptorGpu(slot));

                commandList->DrawIndexedInstanced(command.indexCount, 1, command.indexOffset + indexBase,
                                                  static_cast<INT>(vertexBase), 0);
            }
        }

        auto bindPipeline() noexcept -> void {
            ID3D12DescriptorHeap *heaps[] = {srvHeap};
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootSignature(rootSignature);
            commandList->SetPipelineState(pipelineState);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // Root constants: the projection matrix, pushed from the CPU side copy.
            commandList->SetGraphicsRoot32BitConstants(0, 16, constants.projection, 0);

            if (vertexBuffer != nullptr && indexBuffer != nullptr) {
                D3D12_VERTEX_BUFFER_VIEW vertexView{};
                vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
                vertexView.SizeInBytes = vertexCapacity * sizeof(DrawVertex);
                vertexView.StrideInBytes = sizeof(DrawVertex);
                commandList->IASetVertexBuffers(0, 1, &vertexView);

                D3D12_INDEX_BUFFER_VIEW indexView{};
                indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
                indexView.SizeInBytes = indexCapacity * sizeof(DrawIndex);
                indexView.Format = DXGI_FORMAT_R32_UINT;
                commandList->IASetIndexBuffer(&indexView);
            }

            D3D12_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(backbufferWidth);
            viewport.Height = static_cast<float>(backbufferHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            commandList->RSSetViewports(1, &viewport);

            const D3D12_CPU_DESCRIPTOR_HANDLE target = renderTargetView(SwapChain->GetCurrentBackBufferIndex());
            commandList->OMSetRenderTargets(1, &target, FALSE, nullptr);
        }

        ID3D12Device *Device{nullptr};
        ID3D12CommandQueue *Queue{nullptr};
        IDXGISwapChain3 *SwapChain{nullptr};

        ID3D12CommandAllocator *commandAllocator{nullptr};
        ID3D12GraphicsCommandList *commandList{nullptr};
        ID3D12Fence *fence{nullptr};
        HANDLE fenceEvent{nullptr};
        std::uint64_t fenceValue{0};

        ID3D12RootSignature *rootSignature{nullptr};
        ID3D12PipelineState *pipelineState{nullptr};
        ID3DBlob *vertexBlob{nullptr};
        ID3DBlob *pixelBlob{nullptr};

        /// Shader visible heap every SRV is handed out from. The white texture's
        /// descriptor is the only one the backend owns directly; the rest belong to the
        /// slots of the handle table.
        ID3D12DescriptorHeap *srvHeap{nullptr};
        std::uint32_t srvDescriptorSize{0};
        std::uint32_t descriptorCapacity{0};
        std::uint32_t nextDescriptor{0};

        ID3D12Resource *whiteTexture{nullptr};
        std::uint32_t whiteDescriptor{InvalidDescriptor};

        ID3D12Resource *vertexBuffer{nullptr};
        ID3D12Resource *indexBuffer{nullptr};
        D3D12_RESOURCE_STATES vertexBufferState{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES indexBufferState{D3D12_RESOURCE_STATE_COMMON};
        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;

        /// The frame's single staging buffer, mapped once and written through
        /// `uploadPointer` until it is released or grown.
        ID3D12Resource *uploadBuffer{nullptr};
        void *uploadPointer{nullptr};
        std::uint64_t uploadCapacity{0};

        ID3D12DescriptorHeap *rtvHeap{nullptr};
        std::uint32_t rtvDescriptorSize{0};
        std::uint32_t rtvCount{0};
        std::uint32_t backbufferWidth = 0;
        std::uint32_t backbufferHeight = 0;

        VertexConstants constants{};
        ArrayList<TextureRecord> textures;
        /// Uploads queued by ensureTexture/updateTexture, drained by the next frame.
        ArrayList<PendingUpload> pendingUploads;
        ArrayList<std::uint32_t> freeDescriptors;
    };
} // namespace ReZeroGui
