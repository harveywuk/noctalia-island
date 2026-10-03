#include "launcher/window_management_provider.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "notification/notifications.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

namespace {

  constexpr Logger kLog("launcher-wm");
  // Make Larger/Smaller step, as a share of the work area.
  constexpr double kResizeStep = 0.1;
  // Almost Maximize leaves this share of the work area free around the window.
  constexpr double kAlmostMaximizeInset = 0.05;
  constexpr double kMinWindowSize = 200.0;

  using window_management::Layout;

  constexpr std::array<WindowManagementProvider::Command, 23> kCommands = {{
      {.id = "left-half", .glyph = "box-align-left", .layout = Layout::LeftHalf},
      {.id = "right-half", .glyph = "box-align-right", .layout = Layout::RightHalf},
      {.id = "top-half", .glyph = "box-align-top", .layout = Layout::TopHalf},
      {.id = "bottom-half", .glyph = "box-align-bottom", .layout = Layout::BottomHalf},
      {.id = "top-left-quarter", .glyph = "box-align-top-left", .layout = Layout::TopLeftQuarter},
      {.id = "top-right-quarter", .glyph = "box-align-top-right", .layout = Layout::TopRightQuarter},
      {.id = "bottom-left-quarter", .glyph = "box-align-bottom-left", .layout = Layout::BottomLeftQuarter},
      {.id = "bottom-right-quarter", .glyph = "box-align-bottom-right", .layout = Layout::BottomRightQuarter},
      {.id = "first-third", .glyph = "columns-3", .layout = Layout::FirstThird},
      {.id = "center-third", .glyph = "columns-3", .layout = Layout::CenterThird},
      {.id = "last-third", .glyph = "columns-3", .layout = Layout::LastThird},
      {.id = "first-two-thirds", .glyph = "layout-sidebar-right", .layout = Layout::FirstTwoThirds},
      {.id = "last-two-thirds", .glyph = "layout-sidebar-right", .layout = Layout::LastTwoThirds},
      {.id = "maximize", .glyph = "maximize", .layout = Layout::Maximize},
      {.id = "almost-maximize", .glyph = "arrows-maximize", .layout = Layout::AlmostMaximize},
      {.id = "center", .glyph = "focus-centered", .layout = Layout::Center},
      {.id = "make-larger", .glyph = "arrows-diagonal", .layout = Layout::MakeLarger},
      {.id = "make-smaller", .glyph = "arrows-diagonal-minimize-2", .layout = Layout::MakeSmaller},
      {.id = "toggle-fullscreen", .glyph = "arrows-maximize", .dispatcher = "fullscreen 0"},
      {.id = "toggle-floating", .glyph = "app-window", .dispatcher = "togglefloating {window}"},
      {.id = "pin-window", .glyph = "pin", .dispatcher = "pin {window}"},
      {.id = "next-display", .glyph = "device-desktop", .dispatcher = "movewindow mon:+1"},
      {.id = "previous-display", .glyph = "device-desktop", .dispatcher = "movewindow mon:-1"},
  }};

  [[nodiscard]] std::string titleFor(const WindowManagementProvider::Command& command) {
    return i18n::tr(std::format("launcher.wm.{}.title", command.id));
  }

  [[nodiscard]] std::string keywordsFor(const WindowManagementProvider::Command& command) {
    return i18n::tr(std::format("launcher.wm.{}.keywords", command.id));
  }

  [[nodiscard]] double number(const nlohmann::json& value, std::size_t index, double fallback = 0.0) {
    if (!value.is_array() || index >= value.size() || !value[index].is_number()) {
      return fallback;
    }
    return value[index].get<double>();
  }

  // The monitor's usable area in layout coordinates: its logical size (physical pixels over the
  // scale, swapped for a rotated transform) minus the space reserved for bars.
  [[nodiscard]] std::optional<window_management::Rect>
  workAreaFor(const nlohmann::json& monitors, const nlohmann::json& monitorId) {
    if (!monitors.is_array() || !monitorId.is_number_integer()) {
      return std::nullopt;
    }
    for (const auto& monitor : monitors) {
      if (!monitor.is_object() || monitor.value("id", -1) != monitorId.get<int>()) {
        continue;
      }
      const double scale = std::max(0.1, monitor.value("scale", 1.0));
      double width = monitor.value("width", 0.0) / scale;
      double height = monitor.value("height", 0.0) / scale;
      if ((monitor.value("transform", 0) % 2) != 0) {
        std::swap(width, height);
      }
      const auto& reserved = monitor.contains("reserved") ? monitor["reserved"] : nlohmann::json::array();
      window_management::Rect area{
          .x = monitor.value("x", 0.0) + number(reserved, 0),
          .y = monitor.value("y", 0.0) + number(reserved, 1),
          .width = width - number(reserved, 0) - number(reserved, 2),
          .height = height - number(reserved, 1) - number(reserved, 3),
      };
      if (area.width <= 0.0 || area.height <= 0.0) {
        return std::nullopt;
      }
      return area;
    }
    return std::nullopt;
  }

} // namespace

namespace window_management {

  Rect frameFor(Layout layout, const Rect& area, const Rect& window) {
    const double third = area.width / 3.0;
    const auto at = [&](double x, double y, double width, double height) {
      return Rect{
          .x = std::round(area.x + x),
          .y = std::round(area.y + y),
          .width = std::round(width),
          .height = std::round(height)
      };
    };
    const auto centered = [&](double width, double height) {
      width = std::clamp(width, std::min(kMinWindowSize, area.width), area.width);
      height = std::clamp(height, std::min(kMinWindowSize, area.height), area.height);
      return at((area.width - width) / 2.0, (area.height - height) / 2.0, width, height);
    };
    switch (layout) {
    case Layout::LeftHalf:
      return at(0, 0, area.width / 2.0, area.height);
    case Layout::RightHalf:
      return at(area.width / 2.0, 0, area.width / 2.0, area.height);
    case Layout::TopHalf:
      return at(0, 0, area.width, area.height / 2.0);
    case Layout::BottomHalf:
      return at(0, area.height / 2.0, area.width, area.height / 2.0);
    case Layout::TopLeftQuarter:
      return at(0, 0, area.width / 2.0, area.height / 2.0);
    case Layout::TopRightQuarter:
      return at(area.width / 2.0, 0, area.width / 2.0, area.height / 2.0);
    case Layout::BottomLeftQuarter:
      return at(0, area.height / 2.0, area.width / 2.0, area.height / 2.0);
    case Layout::BottomRightQuarter:
      return at(area.width / 2.0, area.height / 2.0, area.width / 2.0, area.height / 2.0);
    case Layout::FirstThird:
      return at(0, 0, third, area.height);
    case Layout::CenterThird:
      return at(third, 0, third, area.height);
    case Layout::LastThird:
      return at(third * 2.0, 0, third, area.height);
    case Layout::FirstTwoThirds:
      return at(0, 0, third * 2.0, area.height);
    case Layout::LastTwoThirds:
      return at(third, 0, third * 2.0, area.height);
    case Layout::Maximize:
      return at(0, 0, area.width, area.height);
    case Layout::AlmostMaximize:
      return centered(
          area.width * (1.0 - 2.0 * kAlmostMaximizeInset), area.height * (1.0 - 2.0 * kAlmostMaximizeInset)
      );
    case Layout::Center:
      return centered(window.width, window.height);
    case Layout::MakeLarger:
      return centered(window.width + area.width * kResizeStep, window.height + area.height * kResizeStep);
    case Layout::MakeSmaller:
      return centered(window.width - area.width * kResizeStep, window.height - area.height * kResizeStep);
    }
    return window;
  }

} // namespace window_management

std::span<const WindowManagementProvider::Command> WindowManagementProvider::commands() { return kCommands; }

std::string WindowManagementProvider::displayName() const {
  return i18n::tr("launcher.providers.window-management.title");
}

bool WindowManagementProvider::available() const { return m_platform != nullptr && compositors::isHyprland(); }

std::vector<LauncherResult> WindowManagementProvider::search(std::string_view text, bool listAll) const {
  if (!available()) {
    return {};
  }
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  if (needle.empty() && !listAll) {
    return {};
  }
  std::vector<LauncherResult> results;
  for (const auto& command : kCommands) {
    const std::string title = titleFor(command);
    double score = 0.0;
    if (!needle.empty()) {
      const std::string lowerTitle = StringUtils::toLower(title);
      const std::string lowerKeywords = StringUtils::toLower(keywordsFor(command));
      if (!listAll && !launcher_util::wordsMatch(needle, lowerTitle + " " + lowerKeywords)) {
        continue;
      }
      score = FuzzyMatch::score(needle, lowerTitle);
      const double keywordScore = FuzzyMatch::score(needle, lowerKeywords) - 0.5;
      score = std::max(score, keywordScore);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }
    LauncherResult result;
    result.id = std::string(command.id);
    result.title = title;
    result.subtitle = displayName();
    result.glyphName = std::string(command.glyph);
    result.kind = i18n::tr("launcher.kinds.command");
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> WindowManagementProvider::query(std::string_view text) const { return search(text, false); }

std::vector<LauncherResult> WindowManagementProvider::queryPrefixed(std::string_view text) const {
  return search(text, true);
}

bool WindowManagementProvider::run(const Command& command) {
  auto& runtime = m_platform->hyprlandRuntime();
  const auto active = runtime.requestJson("j/activewindow");
  if (!active.has_value() || !active->is_object() || !active->contains("address")) {
    notify::info("Noctalia", i18n::tr("launcher.wm.no-window"), {});
    return false;
  }
  const std::string selector = "address:" + (*active)["address"].get<std::string>();

  std::string request;
  if (command.layout.has_value()) {
    const auto monitors = runtime.requestJson("j/monitors");
    const auto area = monitors.has_value() && active->contains("monitor") ? workAreaFor(*monitors, (*active)["monitor"])
                                                                          : std::nullopt;
    if (!area.has_value()) {
      kLog.warn("{}: no work area for the focused window's monitor", command.id);
      return false;
    }
    const window_management::Rect window{
        .x = number((*active)["at"], 0),
        .y = number((*active)["at"], 1),
        .width = number((*active)["size"], 0),
        .height = number((*active)["size"], 1),
    };
    const auto frame = window_management::frameFor(*command.layout, *area, window);
    // One batch: float the window (a no-op when it already floats), then size and place it.
    request = std::format(
        "[[BATCH]]dispatch setfloating {0}; dispatch resizewindowpixel exact {1} {2},{0}; "
        "dispatch movewindowpixel exact {3} {4},{0}",
        selector, static_cast<long>(frame.width), static_cast<long>(frame.height), static_cast<long>(frame.x),
        static_cast<long>(frame.y)
    );
  } else {
    std::string dispatcher(command.dispatcher);
    if (const auto pos = dispatcher.find("{window}"); pos != std::string::npos) {
      dispatcher.replace(pos, 8, selector);
    }
    if (runtime.configIsLua()) {
      // The Lua-configured Hyprland names the dispatchers it offers differently; use the ones this
      // shell already knows, and leave the rest to the classic names it may still accept.
      if (command.id == "toggle-fullscreen") {
        dispatcher = "hl.dsp.window.fullscreen({mode=\"fullscreen\",action=\"toggle\"})";
      } else if (command.id == "toggle-floating") {
        dispatcher = "hl.dsp.window.float({action=\"toggle\"})";
      }
    }
    request = "dispatch " + dispatcher;
  }

  const auto reply = runtime.request(request);
  if (!reply.has_value()) {
    kLog.warn("{}: Hyprland did not answer", command.id);
    return false;
  }
  if (!reply->starts_with("ok")) {
    kLog.warn("{}: {}", command.id, StringUtils::trim(*reply));
  }
  return true;
}

bool WindowManagementProvider::activate(const LauncherResult& result) {
  if (!available()) {
    return false;
  }
  for (const auto& command : kCommands) {
    if (command.id == result.id) {
      return run(command);
    }
  }
  return false;
}

std::string WindowManagementProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.run-command");
}
