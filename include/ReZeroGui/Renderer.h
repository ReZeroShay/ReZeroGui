#pragma once
#include <concepts>

namespace ReZeroGui {
    // template <typename T>
    // concept RendererTextureLike = requires(T &r, const Image &image, const TextureId id) {
    //     { r.createTexture(image) } noexcept -> std::same_as<TextureId>;
    //     { r.destroyTexture(id) } noexcept;
    // };

    template <typename T>
    concept RendererFrameLike = requires(T &r) {
        { r.beginFrame() } noexcept;
        { r.endFrame() } noexcept;
    };

    template <typename T>
    concept RendererRenderLike = requires(T &r, const void *data) {
        { r.render(data) } noexcept;
    };

    template <typename T>
    // concept RendererLike = RendererFrameLike<T> && RendererTextureLike<T> && RendererRenderLike<T>;
    concept RendererLike = RendererFrameLike<T> && RendererRenderLike<T>;
} // namespace ReZeroGui