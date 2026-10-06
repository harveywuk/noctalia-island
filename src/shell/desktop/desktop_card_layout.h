#pragma once

#include "ui/style.h"

#include <algorithm>
#include <string_view>

namespace desktop_cards {

  inline bool supportsSizePresets(std::string_view type) {
    return type == "stack"
        || type == "weather"
        || type == "calendar"
        || type == "clock"
        || type == "media_player"
        || type == "batteries"
        || type == "screen_time"
        || type == "notes"
        || type == "reminders"
        || type == "shortcuts"
        || type == "contacts"
        || type == "reading_list"
        || type == "journal"
        || type == "tips"
        || type == "photos"
        || type == "news"
        || type == "podcasts"
        || type == "stocks"
        || type == "home"
        || type == "find_my"
        || type == "video_library";
  }

  enum class Size { Classic, Small, Medium, Large };

  inline Size sizeFromSetting(std::string_view value) {
    if (value == "small")
      return Size::Small;
    if (value == "medium")
      return Size::Medium;
    if (value == "large")
      return Size::Large;
    return Size::Classic;
  }

  struct Layout {
    float width;
    float height;
    float scale;
    Size size;
  };

  // Two small tiles and the shell's standard gap fit beside one wide tile.
  inline constexpr float kSmallExtent = 208.0F;
  inline constexpr float kLargeExtent = 2.0F * kSmallExtent + Style::spaceLg;

  inline Layout resolve(Size preset, float scale, float innerWidth, float innerHeight, float padding) {
    const float naturalWidth = (preset == Size::Small ? kSmallExtent : kLargeExtent) * scale;
    const float naturalHeight = (preset == Size::Large ? kLargeExtent : kSmallExtent) * scale;
    const float width = innerWidth > 0.0F ? innerWidth : std::max(1.0F, naturalWidth - 2.0F * padding);
    const float height = innerHeight > 0.0F ? innerHeight : std::max(1.0F, naturalHeight - 2.0F * padding);
    // Resizing changes the information density while keeping normal text at shell size.
    const bool wide = width >= 300.0F * scale;
    // The expanded forecast needs room for its heading and six rows.
    const Size size = wide ? (height >= 384.0F * scale ? Size::Large : Size::Medium) : Size::Small;
    const float fittedScale = std::min({scale, width / 180.0F, height / 180.0F});
    return {width, height, fittedScale, size};
  }

} // namespace desktop_cards
