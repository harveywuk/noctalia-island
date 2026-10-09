#pragma once

// Both icon fonts use the same BMP private-use range. Reserve U+F0000..U+F0FFF
// for Cupertino glyph IDs so scene nodes, caches and saved Tabler codepoints
// remain unambiguous. Only the font renderer converts these IDs back to the
// original Cupertino codepoints. Companion symbols use the same font face with
// their own vacant codepoints, preserving every upstream symbol's shape and ID.
namespace GlyphFont {
  [[nodiscard]] constexpr bool isCupertino(char32_t codepoint) { return codepoint >= 0xF0000 && codepoint <= 0xF0FFF; }

  [[nodiscard]] constexpr char32_t cupertino(char32_t nativeCodepoint) { return 0xF0000 + (nativeCodepoint - 0xF000); }

  [[nodiscard]] constexpr char32_t nativeCodepoint(char32_t codepoint) {
    return isCupertino(codepoint) ? 0xF000 + (codepoint - 0xF0000) : codepoint;
  }
} // namespace GlyphFont
