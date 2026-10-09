#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Glyph names resolve in this order:
// 1. Explicit codepoint literals such as U+F123 or 0xF123.
// 2. Explicit cupertino:, noctalia: or tabler: font-qualified names.
// 3. Noctalia aliases, companion symbols and curated Cupertino replacements.
// 4. Native Tabler icon names for symbols without a Cupertino replacement.
// Cupertino IDs occupy U+F0000..U+F0FFF; BMP Tabler literals remain unchanged.
namespace GlyphRegistry {

  struct TablerGlyphMetadata {
    char32_t codepoint = 0;
    std::string category;
  };

  [[nodiscard]] bool contains(std::string_view name);
  [[nodiscard]] char32_t lookup(std::string_view name);

  // Filled counterparts are opt-in for selected navigation and active controls.
  // Icons without a counterpart keep their original shape.
  [[nodiscard]] char32_t emphasized(char32_t codepoint);
  struct OpticalAdjustment {
    float scale = 1.0F;
    float x = 0.0F;
    float y = 0.0F;
  };
  [[nodiscard]] OpticalAdjustment opticalAdjustment(char32_t codepoint);

  // Full Tabler icon catalog with structured metadata.
  [[nodiscard]] const std::unordered_map<std::string, TablerGlyphMetadata>& tablerGlyphMetadata();
  // Full Tabler icon catalog (loaded from assets/fonts/tabler.json on first registry use).
  [[nodiscard]] const std::unordered_map<std::string, char32_t>& tablerIcons();
  // Cupertino names mapped into the disjoint private-use range (see glyph_font.h).
  [[nodiscard]] const std::unordered_map<std::string, char32_t>& cupertinoIcons();
  // Companion symbols share the Cupertino face, using otherwise vacant codepoints.
  [[nodiscard]] const std::unordered_map<std::string, char32_t>& noctaliaIcons();
  [[nodiscard]] std::optional<std::string_view> categoryFor(std::string_view name);
  // Hand-curated Noctalia alias -> native Tabler icon name map.
  [[nodiscard]] const std::unordered_map<std::string, std::string_view>& aliases();

} // namespace GlyphRegistry
