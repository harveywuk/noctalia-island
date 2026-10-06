#include "shell/desktop/desktop_widget_gallery.h"

#include <glib.h>
#include <sstream>

namespace {
  std::string folded(std::string_view text) {
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
      return std::string(text);
    auto* lower = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
    auto* normalized = g_utf8_normalize(lower, -1, G_NORMALIZE_ALL_COMPOSE);
    const std::string result = normalized ? normalized : lower;
    g_free(normalized);
    g_free(lower);
    return result;
  }
} // namespace

desktop_gallery::Category desktop_gallery::category(std::string_view type) {
  if (type == "clock"
      || type == "calendar"
      || type == "notes"
      || type == "reminders"
      || type == "journal"
      || type == "shortcuts"
      || type == "reading_list"
      || type == "contacts")
    return Category::Productivity;
  if (type == "media_player"
      || type == "photos"
      || type == "video_library"
      || type == "podcasts"
      || type == "audio_visualizer"
      || type == "fancy_audio_visualizer"
      || type == "volume")
    return Category::Media;
  if (type == "batteries" || type == "screen_time" || type == "sysmon")
    return Category::System;
  if (type == "home" || type == "find_my")
    return Category::Home;
  if (type == "weather" || type == "news" || type == "stocks" || type == "tips")
    return Category::Information;
  if (type == "stack" || type == "button" || type == "label" || type == "sticker")
    return Category::Appearance;
  return Category::Plugins;
}

std::string desktop_gallery::favoriteKey(std::string_view type) {
  // State identifiers permit only lowercase letters, digits and underscores.
  // Encode the full ID so punctuation in plugin IDs cannot collide.
  constexpr std::string_view hex = "0123456789abcdef";
  std::string key = "favorite_";
  for (const unsigned char byte : type) {
    key += hex[byte >> 4];
    key += hex[byte & 15];
  }
  return key;
}

bool desktop_gallery::matches(
    std::string_view type, std::string_view label, std::string_view categoryLabel, std::string_view query,
    Category filter, bool favorite
) {
  if ((filter == Category::Favorites && !favorite)
      || (filter != Category::All && filter != Category::Favorites && category(type) != filter))
    return false;
  const auto haystack = folded(std::string(type) + " " + std::string(label) + " " + std::string(categoryLabel));
  std::istringstream words(folded(query));
  for (std::string word; words >> word;)
    if (!haystack.contains(word))
      return false;
  return true;
}

std::string_view desktop_gallery::glyph(std::string_view type) {
  static constexpr std::pair<std::string_view, std::string_view> symbols[] = {
      {"photos", "widget-photos"},
      {"notes", "widget-notes"},
      {"journal", "widget-journal"},
      {"reminders", "widget-reminders"},
      {"shortcuts", "widget-shortcuts"},
      {"contacts", "widget-contacts"},
      {"reading_list", "widget-reading-list"},
      {"tips", "widget-tips"},
      {"news", "widget-news"},
      {"podcasts", "widget-podcasts"},
      {"stocks", "widget-stocks"},
      {"home", "widget-home"},
      {"find_my", "widget-find-my"},
      {"video_library", "widget-videos"},
      {"stack", "widget-stack"},
      {"batteries", "battery-4"},
      {"screen_time", "screen-time"},
      {"calendar", "calendar-event"},
      {"clock", "clock"},
      {"weather", "weather-cloud-sun"},
      {"media_player", "music"},
      {"volume", "volume-high"},
      {"sysmon", "activity"},
      {"button", "hand-click"},
      {"label", "typography"},
      {"sticker", "sticker"},
      {"audio_visualizer", "wave-sine"},
      {"fancy_audio_visualizer", "wave-sine"},
      {"login_box", "lock"},
  };
  for (const auto& [id, symbol] : symbols)
    if (id == type)
      return symbol;
  return "plugin";
}
