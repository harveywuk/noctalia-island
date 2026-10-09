#include "render/text/glyph_registry.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "render/text/glyph_font.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace {

  constexpr Logger kLog("glyph");
  constexpr char32_t kMissingGlyph = 0xF292; // skull

  // Hand-curated alias -> native Tabler icon name map.
  // Use these for semantic shell states and stable Noctalia-facing names.
  // clang-format off
const std::unordered_map<std::string, std::string_view> kAliases = {
    // General
    {"close", "x"},
    {"add", "plus"},
    {"more-vertical", "dots-vertical"},
    {"person", "user"},
    {"info", "info-circle"},
    {"unpin", "pinned-off"},
    {"image", "photo"},
    {"capslock", "keyboard"},
    {"numlock", "keyboard"},
    {"scrolllock", "keyboard"},
    {"plugin", "puzzle"},
    {"official-plugin", "shield-filled"},
    {"package", "box"},

    // Shared shell symbols. Native names and explicit codepoints remain accepted.
    {"apps", "layout-grid"},
    {"edit", "pencil"},
    {"control-center", "adjustments-horizontal"},
    {"focus-on", "moon-filled"},
    {"focus-off", "moon"},
    {"notification-unread", "bell-filled"},
    {"privacy-microphone", "microphone-filled"},
    {"privacy-camera", "camera-filled"},
    {"privacy-screen", "screen-share"},
    {"screen-time", "hourglass"},
    {"device-computer", "device-laptop"},
    {"device-tablet-battery", "device-tablet-filled"},
    {"bluetooth-device-computer", "device-laptop"},
    {"device-ups", "server"},
    {"battery-1", "battery-1-filled"},
    {"battery-2", "battery-2-filled"},
    {"battery-3", "battery-3-filled"},
    {"battery-4", "battery-4-filled"},
    {"widget-photos", "photo"},
    {"widget-notes", "note"},
    {"widget-journal", "book"},
    {"widget-reminders", "list-check"},
    {"widget-shortcuts", "stack-2"},
    {"widget-contacts", "address-book"},
    {"widget-reading-list", "bookmarks"},
    {"widget-tips", "bulb"},
    {"widget-news", "news"},
    {"widget-podcasts", "broadcast"},
    {"widget-stocks", "chart-line"},
    {"widget-home", "home"},
    {"widget-find-my", "radar"},
    {"widget-videos", "movie"},
    {"widget-stack", "stack-2"},

    // Toast / warnings
    {"toast-notice", "circle-check-filled"},
    {"toast-warning", "alert-circle-filled"},
    {"toast-error", "circle-x-filled"},
    {"warning", "alert-triangle-filled"},

    // Media
    {"media-pause", "player-pause-filled"},
    {"media-play", "player-play-filled"},
    {"media-prev", "player-track-prev-filled"},
    {"media-next", "player-track-next-filled"},
    {"shuffle", "arrows-shuffle"},
    {"stop", "player-stop-filled"},
    {"microphone-mute", "microphone-off"},

    // Volume
    {"volume-high", "volume"},
    {"volume-low", "volume-2"},
    {"volume-mute", "volume-off"},
    {"volume-x", "volume-off"},
    {"volume-zero", "volume-3"},

    // Network speed
    {"download-speed", "download"},
    {"upload-speed", "upload"},

    // System monitor
    {"cpu-intensive", "alert-octagon"},
    {"cpu-usage", "brand-speedtest"},
    {"cpu-temperature", "flame"},
    {"gpu-usage", "device-desktop"},
    {"memory", "cpu"},
    {"storage", "database"},
    {"busy", "hourglass-empty"},

    // Power
    {"performance", "gauge-filled"},
    {"balanced", "scale"},
    {"powersaver", "leaf"},
    {"shutdown", "power"},
    {"reboot", "refresh"},
    {"suspend", "moon-filled"},
    {"hibernate", "zzz"},

    // Night light / dark mode
    // A setting sun (evening warmth), so Night Light never shares Do Not Disturb's moon.
    {"nightlight-on", "sunset-filled"},
    {"nightlight-off", "sunset"},
    {"nightlight-forced", "sunset-filled"},
    {"theme-mode", "contrast-filled"},

    // Caffeine (idle inhibitor)
    {"caffeine-on", "mug-filled"},
    {"caffeine-off", "mug"},

    // Brightness / Display
    {"brightness-low", "brightness-down"},
    {"brightness-high", "sun-filled"},

    // Wallpaper / color
    {"wallpaper-selector", "library-photo"},

    // Battery
    {"battery-0", "battery"},
    {"battery-plugged", "battery-charging-2"},

    // Bluetooth devices
    {"bluetooth-device-generic", "bluetooth"},
    {"bluetooth-device-gamepad", "device-gamepad-2"},
    {"bluetooth-device-microphone", "microphone-filled"},
    {"bluetooth-device-headset", "headset"},
    {"bluetooth-device-earbuds", "device-airpods"},
    {"bluetooth-device-headphones", "headphones-filled"},
    {"bluetooth-device-mouse", "mouse-filled"},
    {"bluetooth-device-keyboard", "keyboard-filled"},
    {"bluetooth-device-phone", "device-mobile-filled"},
    {"bluetooth-device-watch", "device-watch-filled"},
    {"bluetooth-device-speaker", "device-speaker-filled"},
    {"bluetooth-device-tv", "device-tv-filled"},

    // Weather
    {"weather-sun", "sun-filled"},
    {"weather-moon", "moon-filled"},
    {"weather-moon-stars", "moon-stars"},
    {"weather-cloud", "cloud-filled"},
    {"weather-cloud-off", "cloud-off"},
    {"weather-cloud-haze", "cloud-fog"},
    {"weather-cloud-lightning", "cloud-bolt"},
    {"weather-cloud-rain", "cloud-rain"},
    {"weather-cloud-snow", "cloud-snow"},
    {"weather-cloud-sun", "cloud-sun"},
    {"weather-sunrise", "sunrise-filled"},
    {"weather-sunset", "sunset-filled"},
};
  // clang-format on

  // Cupertino replacements for shared shell names. The companion set supplies
  // additional devices and states; Tabler remains available for custom glyphs.
  // clang-format off
  const std::unordered_map<std::string, std::string_view> kCupertinoSymbols = {
      {"x", "xmark"}, {"plus", "plus"}, {"minus", "minus"},
      {"check", "checkmark"}, {"search", "search"},
      {"dots", "ellipsis"}, {"dots-vertical", "ellipsis_vertical"},
      {"menu-2", "line_horizontal_3"}, {"user", "person"},
      {"info-circle", "info_circle"}, {"help-circle", "question_circle"},
      {"pencil", "pencil"}, {"trash", "trash"}, {"trash-filled", "trash_fill"},
      {"pin", "pin"}, {"pin-filled", "pin_fill"}, {"pinned-off", "pin_slash"},
      {"photo", "photo"}, {"photo-filled", "photo_fill"},
      {"library-photo", "photo_on_rectangle"},
      {"folder", "folder"}, {"folder-filled", "folder_fill"},
      {"file", "doc"}, {"file-text", "doc_text"},
      {"clipboard", "doc_on_clipboard"}, {"clipboard-filled", "doc_on_clipboard_fill"},
      {"copy", "doc_on_doc"}, {"device-floppy", "floppy_disk"},
      {"link", "link"}, {"external-link", "arrow_up_right_square"},
      {"send", "paperplane"}, {"world", "globe"},
      {"eye", "eye"}, {"eye-off", "eye_slash"},
      {"chevron-left", "chevron_left"}, {"chevron-right", "chevron_right"},
      {"chevron-up", "chevron_up"}, {"chevron-down", "chevron_down"},
      {"arrow-left", "arrow_left"}, {"arrow-right", "arrow_right"},
      {"arrow-up", "arrow_up"}, {"arrow-down", "arrow_down"},
      {"arrow-back-up", "arrow_uturn_left"}, {"arrow-forward-up", "arrow_uturn_right"},
      {"arrows-horizontal", "arrow_left_right"},
      {"refresh", "arrow_clockwise"}, {"reload", "arrow_clockwise"},
      {"rotate-clockwise", "rotate_right"}, {"rotate", "rotate_left"},
      {"zoom-in", "zoom_in"}, {"zoom-out", "zoom_out"},
      {"home", "house"}, {"home-filled", "house_fill"},
      {"settings", "gear_alt"}, {"settings-filled", "gear_alt_fill"},
      {"adjustments-horizontal", "slider_horizontal_3"},
      {"layout-grid", "square_grid_2x2"}, {"layout-grid-filled", "square_grid_2x2_fill"},
      {"grid-dots", "circle_grid_3x3_fill"},
      {"app-window", "macwindow"}, {"stack", "square_stack"},
      {"stack-2", "square_stack_3d_up"},
      {"star", "star"}, {"star-filled", "star_fill"},
      {"heart", "heart"}, {"heart-filled", "heart_fill"},
      {"bell", "bell"}, {"bell-filled", "bell_fill"}, {"bell-off", "bell_slash"},
      {"moon", "moon"}, {"moon-filled", "moon_fill"}, {"moon-stars", "moon_stars"},
      {"sun", "sun_max"}, {"sun-filled", "sun_max_fill"},
      {"brightness-down", "sun_min"}, {"contrast-filled", "circle_lefthalf_fill"},
      {"sunrise", "sunrise"}, {"sunrise-filled", "sunrise_fill"},
      {"sunset", "sunset"}, {"sunset-filled", "sunset_fill"},
      {"cloud", "cloud"}, {"cloud-filled", "cloud_fill"},
      {"cloud-fog", "cloud_fog"}, {"cloud-bolt", "cloud_bolt"},
      {"cloud-rain", "cloud_rain"}, {"cloud-snow", "cloud_snow"},
      {"cloud-sun", "cloud_sun"}, {"wind", "wind"}, {"snowflake", "snow"},
      {"thermometer", "thermometer"},
      {"circle-check", "checkmark_circle"}, {"circle-check-filled", "checkmark_circle_fill"},
      {"circle-x", "xmark_circle"}, {"circle-x-filled", "xmark_circle_fill"},
      {"alert-circle", "exclamationmark_circle"}, {"alert-circle-filled", "exclamationmark_circle_fill"},
      {"alert-triangle", "exclamationmark_triangle"}, {"alert-triangle-filled", "exclamationmark_triangle_fill"},
      {"alert-octagon", "exclamationmark_octagon"},
      {"shield", "shield"}, {"shield-filled", "shield_fill"},
      {"shield-check", "checkmark_shield"}, {"shield-lock", "lock_shield"},
      {"lock", "lock"}, {"lock-filled", "lock_fill"}, {"lock-open", "lock_open"},
      {"music", "music_note_2"},
      {"player-play", "play"}, {"player-play-filled", "play_fill"},
      {"player-pause", "pause"}, {"player-pause-filled", "pause_fill"},
      {"player-stop", "stop"}, {"player-stop-filled", "stop_fill"},
      {"player-track-next", "forward_end"}, {"player-track-next-filled", "forward_end_fill"},
      {"player-track-prev", "backward_end"}, {"player-track-prev-filled", "backward_end_fill"},
      {"player-skip-forward", "forward"}, {"player-skip-back", "backward"},
      {"arrows-shuffle", "shuffle"}, {"repeat", "repeat"}, {"repeat-once", "repeat_1"},
      {"volume", "speaker_2_fill"}, {"volume-2", "speaker_1_fill"},
      {"volume-3", "speaker_fill"}, {"volume-off", "speaker_slash_fill"},
      {"microphone", "mic"}, {"microphone-filled", "mic_fill"}, {"microphone-off", "mic_slash"},
      {"camera", "camera"}, {"camera-filled", "camera_fill"},
      {"video", "videocam"}, {"video-filled", "videocam_fill"},
      {"screenshot", "camera_viewfinder"}, {"crop", "crop"}, {"scissors", "scissors"},
      {"circle", "circle"}, {"circle-filled", "circle_fill"},
      {"square", "square"}, {"square-filled", "square_fill"},
      {"wifi", "wifi"}, {"wifi-off", "wifi_slash"}, {"bluetooth", "bluetooth"},
      {"download", "arrow_down_to_line"}, {"upload", "arrow_up_to_line"},
      {"clock", "clock"}, {"clock-filled", "clock_fill"},
      {"hourglass", "hourglass"}, {"hourglass-filled", "hourglass"}, {"hourglass-empty", "hourglass"},
      {"stopwatch", "stopwatch"}, {"alarm", "alarm"}, {"alarm-filled", "alarm_fill"},
      {"calendar", "calendar"}, {"calendar-event", "calendar"},
      {"device-desktop", "desktopcomputer"}, {"device-laptop", "device_laptop"},
      {"device-mobile", "device_phone_portrait"}, {"device-mobile-filled", "device_phone_portrait"},
      {"device-tv", "tv"}, {"device-tv-filled", "tv_fill"},
      {"device-speaker", "hifispeaker"}, {"device-speaker-filled", "hifispeaker_fill"},
      {"headphones", "headphones"}, {"headphones-filled", "headphones"},
      {"keyboard", "keyboard"}, {"keyboard-filled", "keyboard"},
      {"device-gamepad-2", "gamecontroller"}, {"device-gamepad-2-filled", "gamecontroller_fill"},
      {"power", "power"}, {"zzz", "zzz"},
      {"gauge", "gauge"}, {"gauge-filled", "gauge"}, {"brand-speedtest", "speedometer"},
      {"flame", "flame"}, {"flame-filled", "flame_fill"},
      {"bolt", "bolt"}, {"bolt-filled", "bolt_fill"},
      {"wave-sine", "waveform"},
      {"note", "doc_text"}, {"book", "book"}, {"bookmarks", "bookmark"},
      {"list-check", "text_badge_checkmark"}, {"list", "list_bullet"},
      {"address-book", "person_crop_rectangle"},
      {"bulb", "lightbulb"}, {"bulb-filled", "lightbulb_fill"},
      {"news", "doc_richtext"}, {"broadcast", "antenna_radiowaves_left_right"},
      {"chart-line", "graph_square"}, {"radar", "scope"}, {"movie", "film"},
      {"map", "map"}, {"map-pin", "map_pin"}, {"map-pin-off", "map_pin_slash"},
      {"color-picker", "eyedropper"}, {"brush", "paintbrush"}, {"box", "cube_box"},
      {"activity", "waveform_path_ecg"}, {"temperature", "thermometer"},
      {"notification", "bell"}, {"wallpaper", "photo_on_rectangle"},
      {"wifi-exclamation", "wifi_exclamationmark"},
      {"file-check", "doc_checkmark"}, {"copy-plus", "plus_square_on_square"},
      {"flip-horizontal", "arrow_left_right"}, {"flip-vertical", "arrow_up_down"},
      {"stack-back", "square_fill_on_square_fill"}, {"stack-front", "square_on_square"},
      {"bug", "ant"}, {"brightness-up", "sun_max"},
      {"logout", "square_arrow_right"}, {"history", "gobackward"},
      {"filter", "line_horizontal_3_decrease"}, {"adjustments", "slider_horizontal_3"},
      {"share", "square_arrow_up"}, {"briefcase", "briefcase"},
      {"sparkles", "sparkles"}, {"wand", "wand_stars"},
      {"clipboard-text", "doc_on_clipboard"}, {"clipboard-copy", "doc_on_doc"},
      {"list-details", "list_bullet"}, {"messages", "bubble_left_bubble_right"},
      {"mood-smile", "smiley"}, {"arrows-maximize", "arrow_up_left_arrow_down_right"},
      {"arrows-minimize", "arrow_down_right_arrow_up_left"},
      {"arrow-big-up", "shift"}, {"pinned", "pin"},
      // Settings, pickers, launcher providers and utility surfaces share the same
      // symbol family as the Island. Keep brand marks and custom Tabler choices.
      {"palette", "color_filter"}, {"location", "location"},
      {"layout-bottombar", "rectangle_dock"},
      {"layout-sidebar", "sidebar_left"}, {"layout-sidebar-right", "sidebar_right"},
      {"layout-board", "rectangle_split_3x1"}, {"layout-dashboard", "rectangle_grid_2x2"},
      {"columns-3", "rectangle_split_3x1"},
      {"dots-circle-horizontal", "ellipsis_circle"},
      {"circle-dot", "smallcircle_circle"}, {"checkbox", "checkmark_square"},
      {"typography", "textformat"}, {"typeface", "textformat"}, {"letter-t", "textformat"},
      {"sticker", "square_on_circle"}, {"hand-click", "hand_point_left"},
      {"sort-a-z", "sort_down"}, {"sort-z-a", "sort_up"},
      {"sort-ascending-2", "sort_up"}, {"sort-descending-2", "sort_down"},
      {"sort-ascending-2-filled", "sort_up"}, {"sort-descending-2-filled", "sort_down"},
      {"arrows-random", "shuffle"}, {"arrows-exchange", "arrow_right_arrow_left"},
      {"arrow-up-right", "arrow_up_right"}, {"arrows-move", "move"},
      {"arrows-diagonal", "arrow_up_left_arrow_down_right"},
      {"arrows-diagonal-minimize-2", "arrow_down_right_arrow_up_left"},
      {"maximize", "arrow_up_left_arrow_down_right"}, {"focus-centered", "viewfinder"},
      {"layers-intersect", "square_on_square"},
      {"droplet", "drop"}, {"temperature-sun", "thermometer_sun"},
      {"antenna-bars-5", "antenna_radiowaves_left_right"},
      {"bolt-off", "bolt_slash"}, {"bell-x", "bell_slash"},
      {"login", "square_arrow_left"}, {"phone", "phone"}, {"player-eject", "eject"},
      {"command", "command"}, {"code", "chevron_left_slash_chevron_right"},
      {"script", "doc_text"}, {"quotes", "text_quote"}, {"file-plus", "doc_append"},
      {"file-type-pdf", "doc_richtext"}, {"file-zip", "archivebox"},
      {"text-recognition", "doc_text_viewfinder"}, {"text-grammar", "textformat_abc_dottedunderline"},
      {"mood-smile-beam", "smiley"}, {"world-search", "globe"},
      {"tag", "tag"}, {"tags", "tags"}, {"flag", "flag"}, {"paw", "paw"},
      {"school", "book"}, {"tool", "wrench"}, {"restore", "arrow_counterclockwise"},
      {"notes", "doc_text"}, {"rectangle", "rectangle"}, {"pointer", "cursor_rays"},
      {"language", "globe"}, {"circuit-pushbutton", "hand_point_left"},
      {"stack-pop", "square_stack_3d_up"},
  };
  // clang-format on

  [[nodiscard]] std::optional<char32_t> parseCodepointLiteral(std::string_view value) {
    if (value.size() < 3) {
      return std::nullopt;
    }

    std::string_view hex;
    if ((value[0] == 'U' || value[0] == 'u') && value[1] == '+') {
      hex = value.substr(2);
    } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
      hex = value.substr(2);
    } else {
      return std::nullopt;
    }

    if (hex.empty()) {
      return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    const auto* begin = hex.data();
    const auto* end = begin + hex.size();
    const auto result = std::from_chars(begin, end, codepoint, 16);
    if (result.ec != std::errc{} || result.ptr != end || codepoint == 0 || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    return static_cast<char32_t>(codepoint);
  }

  [[nodiscard]] std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata> loadTablerMetadata() {
    std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata> icons;
    const std::filesystem::path path = paths::assetPath("fonts/tabler.json");
    std::ifstream file(path);
    if (!file.is_open()) {
      kLog.warn("failed to open Tabler glyph metadata: {}", path.string());
      return icons;
    }

    try {
      const auto root = nlohmann::json::parse(file);
      if (!root.is_object()) {
        kLog.warn("Tabler glyph metadata is not an object: {}", path.string());
        return icons;
      }

      icons.reserve(root.size());
      for (const auto& [name, value] : root.items()) {
        if (!value.is_object()) {
          continue;
        }
        const auto codepointIt = value.find("codepoint");
        const auto categoryIt = value.find("category");
        if (codepointIt == value.end()
            || categoryIt == value.end()
            || !codepointIt->is_string()
            || !categoryIt->is_string()) {
          continue;
        }
        const std::string codepoint = codepointIt->get<std::string>();
        if (auto parsed = parseCodepointLiteral(codepoint)) {
          icons.emplace(
              name,
              GlyphRegistry::TablerGlyphMetadata{
                  .codepoint = *parsed,
                  .category = categoryIt->get<std::string>(),
              }
          );
        }
      }
      kLog.debug("loaded {} Tabler glyph names from {}", icons.size(), path.string());
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse Tabler glyph metadata '{}': {}", path.string(), e.what());
    }
    return icons;
  }

  [[nodiscard]] const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata>& tablerMetadata() {
    static const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata> icons = loadTablerMetadata();
    return icons;
  }

  [[nodiscard]] const std::unordered_map<std::string, char32_t>& tablerIcons() {
    static const std::unordered_map<std::string, char32_t> icons = [] {
      std::unordered_map<std::string, char32_t> flat;
      const auto& metadata = tablerMetadata();
      flat.reserve(metadata.size());
      for (const auto& [name, entry] : metadata) {
        flat.emplace(name, entry.codepoint);
      }
      return flat;
    }();
    return icons;
  }

  [[nodiscard]] std::unordered_map<std::string, char32_t> loadCupertinoIcons(std::string_view asset) {
    std::unordered_map<std::string, char32_t> icons;
    const auto path = paths::assetPath(asset);
    std::ifstream file(path);
    if (!file.is_open()) {
      kLog.warn("failed to open Cupertino glyph metadata: {}", path.string());
      return icons;
    }
    try {
      const auto root = nlohmann::json::parse(file);
      if (!root.is_object())
        return icons;
      for (const auto& [name, value] : root.items()) {
        if (!value.is_string())
          continue;
        if (const auto cp = parseCodepointLiteral(value.get<std::string>()); cp && *cp >= 0xF000 && *cp <= 0xFFFF)
          icons.emplace(name, GlyphFont::cupertino(*cp));
      }
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse Cupertino glyph metadata '{}': {}", path.string(), e.what());
    }
    return icons;
  }

  [[nodiscard]] std::optional<char32_t> resolve(std::string_view name) {
    if (const auto cp = parseCodepointLiteral(name))
      return cp;
    const auto& tabler = tablerIcons();
    const auto& cupertino = GlyphRegistry::cupertinoIcons();
    const auto& companion = GlyphRegistry::noctaliaIcons();
    if (name.starts_with("tabler:")) {
      const auto it = tabler.find(std::string(name.substr(7)));
      return it == tabler.end() ? std::nullopt : std::optional{it->second};
    }
    if (name.starts_with("cupertino:")) {
      const auto it = cupertino.find(std::string(name.substr(10)));
      return it == cupertino.end() ? std::nullopt : std::optional{it->second};
    }
    if (name.starts_with("noctalia:")) {
      const auto it = companion.find(std::string(name.substr(9)));
      return it == companion.end() ? std::nullopt : std::optional{it->second};
    }
    std::string key{name};
    if (const auto alias = kAliases.find(key); alias != kAliases.end())
      key = alias->second;
    if (const auto it = companion.find(key); it != companion.end())
      return it->second;
    if (const auto symbol = kCupertinoSymbols.find(key); symbol != kCupertinoSymbols.end()) {
      if (const auto it = cupertino.find(std::string(symbol->second)); it != cupertino.end())
        return it->second;
    }
    const auto it = tabler.find(key);
    return it == tabler.end() ? std::nullopt : std::optional{it->second};
  }

} // namespace

bool GlyphRegistry::contains(std::string_view name) { return resolve(name).has_value(); }

char32_t GlyphRegistry::lookup(std::string_view name) {
  if (auto codepoint = resolve(name)) {
    return *codepoint;
  }
  kLog.warn("missing glyph: {}", name);
  return kMissingGlyph;
}

char32_t GlyphRegistry::emphasized(char32_t codepoint) {
  static const auto variants = [] {
    std::unordered_map<char32_t, char32_t> result;
    const auto& icons = tablerIcons();
    for (const auto& [name, cp] : icons)
      if (const auto it = icons.find(name + "-filled"); it != icons.end())
        result.emplace(cp, it->second);
    const auto& cupertino = cupertinoIcons();
    for (const auto& [name, cp] : cupertino)
      if (const auto it = cupertino.find(name + "_fill"); it != cupertino.end())
        result.emplace(cp, it->second);
    const auto& companion = noctaliaIcons();
    for (const auto& [name, cp] : companion)
      if (const auto it = companion.find(name + "-filled"); it != companion.end())
        result.emplace(cp, it->second);
    return result;
  }();
  const auto it = variants.find(codepoint);
  return it == variants.end() ? codepoint : it->second;
}

GlyphRegistry::OpticalAdjustment GlyphRegistry::opticalAdjustment(char32_t codepoint) {
  // Dense silhouettes need slightly more breathing room at menu-bar sizes.
  // Play triangles sit a little forward of their mathematical bounding-box center.
  static const auto adjustments = [] {
    std::unordered_map<char32_t, OpticalAdjustment> result;
    const auto add = [&](std::string_view name, OpticalAdjustment adjustment) {
      const auto& icons = tablerIcons();
      if (const auto it = icons.find(std::string(name)); it != icons.end())
        result.emplace(it->second, adjustment);
    };
    for (const auto name :
         {"home-filled", "settings-filled", "headphones-filled", "device-speaker-filled", "device-mobile-filled",
          "device-tablet-filled", "device-tv-filled", "keyboard-filled", "mouse-filled", "camera-filled",
          "microphone-filled", "bell-filled", "moon-filled", "sun-filled", "lock-filled", "clipboard-filled",
          "hourglass-filled"})
      add(name, {.scale = .94F});
    add("player-play-filled", {.scale = .92F, .x = .035F});
    add("player-pause-filled", {.scale = .90F});
    add("player-stop-filled", {.scale = .88F});
    add("player-track-next-filled", {.scale = .94F, .x = .02F});
    add("player-track-prev-filled", {.scale = .94F, .x = -.02F});
    const auto addCupertino = [&](std::string_view name, OpticalAdjustment adjustment) {
      if (const auto it = cupertinoIcons().find(std::string(name)); it != cupertinoIcons().end())
        result.emplace(it->second, adjustment);
    };
    for (const auto name : {"bell", "bell_fill", "house", "house_fill", "wifi", "wifi_slash"})
      addCupertino(name, {.scale = .88F});
    addCupertino("xmark", {.scale = .85F});
    addCupertino("play_fill", {.scale = .98F, .x = .035F});
    addCupertino("pause_fill", {.scale = 1.0F});
    addCupertino("stop_fill", {.scale = .9F});
    addCupertino("forward_end_fill", {.scale = .94F, .x = .02F});
    addCupertino("backward_end_fill", {.scale = .94F, .x = -.02F});
    return result;
  }();
  const auto it = adjustments.find(codepoint);
  return it == adjustments.end() ? OpticalAdjustment{.scale = GlyphFont::isCupertino(codepoint) ? .94F : 1.0F}
                                 : it->second;
}

const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata>& GlyphRegistry::tablerGlyphMetadata() {
  return ::tablerMetadata();
}

const std::unordered_map<std::string, char32_t>& GlyphRegistry::tablerIcons() { return ::tablerIcons(); }

const std::unordered_map<std::string, char32_t>& GlyphRegistry::cupertinoIcons() {
  static const auto icons = loadCupertinoIcons("fonts/cupertino.json");
  return icons;
}

const std::unordered_map<std::string, char32_t>& GlyphRegistry::noctaliaIcons() {
  static const auto icons = loadCupertinoIcons("fonts/noctalia-symbols.json");
  return icons;
}

std::optional<std::string_view> GlyphRegistry::categoryFor(std::string_view name) {
  if (name.starts_with("noctalia:"))
    return contains(name) ? std::optional<std::string_view>{"Noctalia"} : std::nullopt;
  if (name.starts_with("cupertino:"))
    return contains(name) ? std::optional<std::string_view>{"Cupertino"} : std::nullopt;
  if (name.starts_with("tabler:"))
    name.remove_prefix(7);
  const auto& metadata = tablerGlyphMetadata();
  const std::string key{name};
  if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
    if (const auto it = metadata.find(std::string(alias->second)); it != metadata.end()) {
      return it->second.category;
    }
    return std::nullopt;
  }

  if (const auto it = metadata.find(key); it != metadata.end()) {
    return it->second.category;
  }
  return std::nullopt;
}

const std::unordered_map<std::string, std::string_view>& GlyphRegistry::aliases() { return kAliases; }
