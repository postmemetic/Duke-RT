#include "startup_font.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace StartupFont
{
namespace
{
struct GlyphRecord
{
	uint16_t offset;
	uint8_t width;
	uint8_t height;
};

template<size_t N> struct PixelStore
{
	uint8_t pixels[N - 1] = {};
};

// The packaged Duke glyphs use palette band 129..143 for their dark outline
// through bright face. These are explicit startup shades, not a game palette
// lookup or an interpretation of raw palette indices as intensity. Empty patch
// pixels and index 0 become black/transparent. The host supplies the tint.
constexpr uint8_t kStartupShades[16] =
{
	0, 14, 25, 42, 64, 88, 112, 138, 160, 180, 200, 218, 232, 242, 250, 255
};

template<size_t N> constexpr PixelStore<N> DecodeShades(const char (&encoded)[N])
{
	PixelStore<N> output;
	for (size_t i = 0; i < N - 1; ++i)
	{
		const unsigned index = encoded[i] <= '9' ? encoded[i] - '0' : encoded[i] - 'a' + 10;
		output.pixels[i] = kStartupShades[index];
	}
	return output;
}

#include "startup_font_data.inc"

constexpr Font kDuke13 = { "duke13", 20, 5 };
constexpr Font kDuke15 = { "duke15", 20, 5 };

bool IsFont(const Font& font, const Font& candidate) noexcept
{
	return font.id != nullptr && std::strcmp(font.id, candidate.id) == 0;
}
}

const Font* Find(const char* id) noexcept
{
	if (id == nullptr) return nullptr;
	if (std::strcmp(id, kDuke13.id) == 0) return &kDuke13;
	if (std::strcmp(id, kDuke15.id) == 0) return &kDuke15;
	return nullptr;
}

bool GetGlyph(const Font& font, uint32_t codepoint, Glyph& glyph) noexcept
{
	glyph = {};
	const bool duke13 = IsFont(font, kDuke13);
	if (!duke13 && !IsFont(font, kDuke15)) return false;
	if (codepoint == ' ')
	{
		glyph.advance = font.spaceWidth;
		return true;
	}
	if (codepoint >= 'a' && codepoint <= 'z') codepoint -= 'a' - 'A';
	if (codepoint < 33 || codepoint > 126) return false;
	const GlyphRecord& record = (duke13 ? kDuke13Glyphs : kDuke15Glyphs)[codepoint - 33];
	if (record.width == 0) return false;
	glyph.width = record.width;
	glyph.height = record.height;
	glyph.advance = record.width;
	glyph.pixels = (duke13 ? kDuke13Pixels.pixels : kDuke15Pixels.pixels) + record.offset;
	return true;
}

int MeasureText(const Font& font, const char* text) noexcept
{
	if (text == nullptr || (!IsFont(font, kDuke13) && !IsFont(font, kDuke15))) return -1;
	int width = 0;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p)
	{
		Glyph glyph;
		if (!GetGlyph(font, *p, glyph) || glyph.advance > std::numeric_limits<int>::max() - width) return -1;
		width += glyph.advance;
	}
	return width;
}
}
