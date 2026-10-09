#include "capture/window_capture.h"

#include "compositors/hyprland/hyprland_window_id.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <tuple>

namespace capture {
  std::optional<std::vector<WindowTarget>> parseHyprlandCaptureWindows(std::string_view text) {
    const auto clients = nlohmann::json::parse(text, nullptr, false);
    if (!clients.is_array())
      return std::nullopt;
    std::vector<WindowTarget> result;
    for (const auto& client : clients) {
      try {
        // Older IPC without visibility cannot safely distinguish hidden workspaces.
        if (!client.contains("visible"))
          return std::nullopt;
        if (!client.value("visible", false) || !client.value("mapped", false) || client.value("hidden", true))
          continue;
        const auto id = client.at("address").get<std::string>();
        std::uint64_t address = 0;
        if (!id.starts_with("0x"))
          continue;
        const auto parsed = std::from_chars(id.data() + 2, id.data() + id.size(), address, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || address == 0)
          continue;
        const auto& at = client.at("at");
        const auto& size = client.at("size");
        LogicalRect bounds{at.at(0).get<int>(), at.at(1).get<int>(), size.at(0).get<int>(), size.at(1).get<int>()};
        if (bounds.width < 2
            || bounds.height < 2
            || bounds.width > 32768
            || bounds.height > 32768
            || bounds.x < -1048576
            || bounds.x > 1048576
            || bounds.y < -1048576
            || bounds.y > 1048576)
          continue;
        const int order = client.value("focusHistoryID", -1);
        // Hyprland also reports tiled windows as allowed over fullscreen when
        // no fullscreen window is present. That flag must not raise tiled windows.
        const int layer = client.value("fullscreen", 0) > 0 ? 2
            : client.value("floating", false)               ? (client.value("allowedOverFullscreen", false) ? 3 : 1)
                                                            : 0;
        auto title = client.value("title", std::string{});
        if (title.empty())
          title = client.value("class", std::string{});
        std::ranges::replace_if(title, [](unsigned char c) { return c < 32 || c == 127; }, ' ');
        result.push_back(
            {compositors::hyprland::formatWindowAddress(address), bounds, layer,
             order < 0 ? std::numeric_limits<int>::max() : order, std::move(title)}
        );
      } catch (const nlohmann::json::exception&) {
        continue;
      }
    }
    std::stable_sort(result.begin(), result.end(), [](const WindowTarget& a, const WindowTarget& b) {
      return std::tuple{-a.layer, a.focusOrder} < std::tuple{-b.layer, b.focusOrder};
    });
    return result;
  }

  const WindowTarget* windowAt(const std::vector<WindowTarget>& windows, double x, double y) {
    const auto found = std::ranges::find_if(windows, [=](const WindowTarget& window) {
      const auto& b = window.bounds;
      return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
    });
    return found == windows.end() ? nullptr : &*found;
  }
} // namespace capture
