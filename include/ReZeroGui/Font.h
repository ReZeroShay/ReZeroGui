#pragma once
#include <ReZeroGui/ReZeroLib.h>
#include <ReZeroGui/FontAssets/ProggyForever-Regular.h>
#include <ReZeroTrueType/ReZeroTrueType.h>
namespace ReZeroGui {
    using FontId = std::uint32_t;
    constexpr FontId InvalidFont = std::numeric_limits<FontId>::max();

    constexpr float defaultFontSizePixels = 22.0f;
    /// Device space placement of one glyph.
    ///
    /// position0/position1 are the top-left and bottom-right corners of the quad
    /// in UI pixels; uv0/uv1 are the matching corners inside the font atlas. Both
    /// are spelled as (minimum, maximum) so they can be handed straight to
    /// DrawList::addTexturedQuad.
    struct GlyphQuad {
        Vector2 position0;
        Vector2 position1;
        Vector2 uv0;
        Vector2 uv1;
    };

    /// Owns the CPU side of the glyph atlases and answers every text query.
    ///
    /// The face bytes are only borrowed (see FontParams), so they have to stay
    /// alive until build() returns. The atlases themselves are owned here and are
    /// exposed through atlasImage() so the backend can upload them as a texture.
    struct FontAtlas {
        struct FontParams {
            const std::uint8_t *data = nullptr; ///< TTF/OTF bytes, non-owning.
            std::size_t size = 0;
            float sizePixels = defaultFontSizePixels;
            std::uint32_t firstCodepoint = 0x20;
            std::uint32_t lastCodepoint = 0x7E;
        };

        struct Font {
            FontParams params;
            /// Distance from the baseline to the top of the line, in pixels.
            float ascent = 0.0f;
            /// Distance from the baseline to the bottom of the line. Negative for
            /// the usual TrueType vertical metrics.
            float descent = 0.0f;
            /// ascent - descent + lineGap, i.e. the baseline to baseline distance.
            float lineHeight = 0.0f;
            ReZeroTrueType::PackedAtlas atlas;

            explicit Font(ReZeroTrueType::AllocatorView source) noexcept : atlas(source) {}
        };
        /// The TrueType library has its own AllocatorView, so the UI allocator is
        /// wrapped once here. `source` has to outlive the atlas, which is why the
        /// view is built from the member below rather than from the constructor
        /// argument.
        explicit FontAtlas(AllocatorView allocatorView) noexcept : allocator(allocatorView), ttfAllocator(allocator) {}

        FontAtlas(const FontAtlas &) = delete;
        FontAtlas &operator=(const FontAtlas &) = delete;

        /// Registers a face. Nothing is rasterized until build(), so a batch of
        /// faces only pays for one atlas allocation round.
        auto addFont(const FontParams &params) noexcept -> FontId {
            if (params.data == nullptr || params.size == 0) {
                return InvalidFont;
            }
            if (params.lastCodepoint < params.firstCodepoint) {
                return InvalidFont;
            }
            fonts.emplace_back(ttfAllocator);
            fonts.back().params = params;
            return static_cast<FontId>(fonts.size() - 1);
        }

        auto loadDefaultFont(float sizePixel = defaultFontSizePixels) {
            FontParams params;
            params.data = ProggyForever_Regular_ttf;
            params.size = ProggyForever_Regular_ttf_len;
            params.sizePixels = sizePixel;
            defaultFontId = addFont(params);
        }

        auto build() noexcept -> bool {
            if (built) {
                return true;
            }
            if (fonts.empty()) {
                return false;
            }
            for (Font &font : fonts) {
                if (!rasterize(font)) {
                    return false;
                }
            }
            if (defaultFontId == InvalidFont) {
                defaultFontId = 0;
            }
            built = true;
            return true;
        }
        auto isBuilt() const noexcept -> bool { return built; }
        auto defaultFont() const noexcept -> FontId { return defaultFontId; }
        auto ascent(FontId id) const noexcept -> float {
            const Font *font = fontFor(id);
            return font != nullptr ? font->ascent : 0.0f;
        }

        auto descent(FontId id) const noexcept -> float {
            const Font *font = fontFor(id);
            return font != nullptr ? font->descent : 0.0f;
        }

        auto lineHeight(FontId id) const noexcept -> float {
            const Font *font = fontFor(id);
            REZERO_ASSERT(font != nullptr);
            return font != nullptr ? font->lineHeight : 0.0f;
        }

        /// Horizontal pen movement for one code point, in pixels. Zero for
        /// code points the atlas does not carry.
        auto glyphAdvance(FontId id, std::uint32_t codepoint) const noexcept -> float {
            const Font *font = fontFor(id);
            if (font == nullptr) {
                return 0.0f;
            }
            const ReZeroTrueType::GlyphMetric *glyph = font->atlas.find(codepoint);
            return glyph != nullptr ? glyph->advance : 0.0f;
        }

        /// Places one glyph and advances the pen.
        ///
        /// On entry `penX` is the pen position and `baselineY` the baseline the
        /// glyph sits on. On success `penX` is advanced by the glyph's advance
        /// width while `baselineY` is left alone.
        ///
        /// Blank glyphs (spaces, and code points the face has no outline for)
        /// report success with a quad whose UVs are empty so the pen still moves;
        /// callers are expected to test the UVs before emitting geometry, which is
        /// what Context::renderText does.
        auto getGlyphQuad(FontId id, std::uint32_t codepoint, float *penX, float *baselineY,
                          GlyphQuad &quad) const noexcept -> bool {
            const Font *font = fontFor(id);
            if (font == nullptr || penX == nullptr || baselineY == nullptr) {
                return false;
            }

            const ReZeroTrueType::GlyphMetric *glyph = font->atlas.find(codepoint);
            if (glyph == nullptr) {
                return false;
            }

            const float pen = *penX;
            *penX = pen + glyph->advance;
            quad = GlyphQuad{};

            if (glyph->blank || glyph->w == 0 || glyph->h == 0) {
                return true;
            }

            // Snap the glyph origin to whole pixels. The atlas was rasterized at
            // exactly sizePixels, so a 1:1 texel-to-pixel quad resamples nothing
            // and every pixel centre lands on a texel centre. The pen keeps its
            // fractional part, so snapping the drawn quad does not make advances
            // drift.
            const float originX = std::round(pen);
            const float originY = std::round(*baselineY);
            quad.position0 = Vector2(originX + static_cast<float>(glyph->ox), originY + static_cast<float>(glyph->oy));
            quad.position1 = Vector2(quad.position0.x + static_cast<float>(glyph->w),
                                     quad.position0.y + static_cast<float>(glyph->h));
            quad.uv0 = Vector2(glyph->u0, glyph->v0);
            quad.uv1 = Vector2(glyph->u1, glyph->v1);
            return true;
        }

        /// The atlas image backing a face, or null when it is not built yet.
        /// Pixels are RGBA8 with white RGB and coverage in alpha.
        auto atlasImage(FontId id) const noexcept -> const ReZeroTrueType::Image * {
            const Font *font = fontFor(id);
            if (font == nullptr) {
                return nullptr;
            }
            const ReZeroTrueType::Image &grid = font->atlas.grid;
            if (grid.width == 0 || grid.height == 0 || grid.data() == nullptr) {
                return nullptr;
            }
            return &grid;
        }

      private:
        auto isValid(FontId id) const noexcept -> bool {
            return id != InvalidFont && static_cast<std::size_t>(id) < fonts.size();
        }

        auto fontFor(FontId id) const noexcept -> const Font * {
            return isValid(id) ? &fonts[static_cast<std::size_t>(id)] : nullptr;
        }
        auto rasterize(Font &font) noexcept -> bool {
            // The TrueType reader only reads the font bytes but still asks for a
            // mutable span, while FontParams deliberately keeps the caller's data
            // const so registration never implies write access.
            auto *bytes = const_cast<std::uint8_t *>(font.params.data);
            auto truetype = ReZeroTrueType::TrueType::loadTrueTyperomMemory(ttfAllocator, {bytes, font.params.size});
            if (!truetype) {
                return false;
            }

            ReZeroTrueType::FontPackerParams packerParams;
            packerParams.pixelSize = font.params.sizePixels;

            ReZeroTrueType::FontPacker packer(ttfAllocator, *truetype, packerParams);
            packer.includeRange(font.params.firstCodepoint, font.params.lastCodepoint);

            auto packed = packer.pack();
            if (!packed) {
                return false;
            }
            font.atlas = std::move(*packed);

            const float pixelSize = font.params.sizePixels;
            font.ascent = truetype->unitsToPixel(static_cast<float>(truetype->ascent), pixelSize);
            font.descent = truetype->unitsToPixel(static_cast<float>(truetype->descent), pixelSize);
            font.lineHeight = truetype->lineHeight(pixelSize);
            return true;
        }
        AllocatorView allocator;
        ReZeroTrueType::AllocatorView ttfAllocator;
        InplaceVector<Font, 3> fonts;
        FontId defaultFontId = InvalidFont;
        bool built = false;
    };
} // namespace ReZeroGui
