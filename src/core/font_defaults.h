#pragma once

namespace font_defaults {

  // Shell UI font, as a fontconfig/Pango family list. SF Pro is never bundled, but it is
  // preferred where the user has installed it; Inter is the closest open substitute, and
  // the generic alias keeps text rendering on systems with neither.
  inline constexpr const char* kFamily = "SF Pro Text, SF Pro, Inter, sans-serif";

} // namespace font_defaults
