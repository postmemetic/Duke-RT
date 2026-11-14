#pragma once

#include <cstdint>

// Small immutable CPU fonts for the startup window. They deliberately do not
// depend on FFont, TexMan, game archives, palettes, or graphics initialization.
namespace StartupFont
{
struct Glyph
{
	int width = 0;
	int height = 0;
	int leftOffset = 0;
	int topOffset = 0;
	int advance = 0;
	// Row-major shade/coverage, 0 transparent and 255 full foreground color.
	const uint8_t* pixels = nullptr;
};

struct Font
{
	const char* id;
	int lineHeight;
	int spaceWidth;
};

// Unknown IDs (including "default") request the host's ordinary text fallback.
const Font* Find(const char* id) noexcept;

// Lowercase ASCII uses its menu uppercase equivalent. Missing characters are
// reported rather than silently replaced, so the host can fall back per line.
bool GetGlyph(const Font& font, uint32_t codepoint, Glyph& glyph) noexcept;

// Returns -1 for unsupported text or overflow. No allocation or mutable cache.
int MeasureText(const Font& font, const char* text) noexcept;
}
