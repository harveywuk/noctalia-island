#pragma once

#include <array>
#include <string>
#include <string_view>

namespace desktop_gallery {
  enum class Category { All, Favorites, Productivity, Media, System, Home, Information, Appearance, Plugins };
  struct Filter {
    Category category;
    const char* label;
  };
  inline constexpr std::array filters{
      Filter{Category::All, "desktop-widgets.editor.gallery.all"},
      Filter{Category::Favorites, "desktop-widgets.editor.gallery.favorites"},
      Filter{Category::Productivity, "desktop-widgets.editor.gallery.productivity"},
      Filter{Category::Media, "desktop-widgets.editor.gallery.media"},
      Filter{Category::System, "desktop-widgets.editor.gallery.system"},
      Filter{Category::Home, "desktop-widgets.editor.gallery.home"},
      Filter{Category::Information, "desktop-widgets.editor.gallery.information"},
      Filter{Category::Appearance, "desktop-widgets.editor.gallery.appearance"},
      Filter{Category::Plugins, "desktop-widgets.editor.gallery.plugins"},
  };
  [[nodiscard]] std::string_view glyph(std::string_view type);
  [[nodiscard]] Category category(std::string_view type);
  [[nodiscard]] std::string favoriteKey(std::string_view type);
  [[nodiscard]] bool matches(
      std::string_view type, std::string_view label, std::string_view categoryLabel, std::string_view query,
      Category filter, bool favorite
  );
} // namespace desktop_gallery
