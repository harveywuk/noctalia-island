#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace island {
  // Use connector names, never wl_output pointers, across unplug/reconnect.
  struct PreviewTarget {
    std::string output;

    void reconcile(const std::vector<std::string>& available, std::string_view focused) {
      if (std::ranges::find(available, output) != available.end())
        return;
      output = std::ranges::find(available, focused) != available.end() ? std::string(focused)
          : available.empty()                                           ? ""
                                                                        : available.front();
    }

    bool matches(std::string_view setting, std::string_view current) const {
      if (setting == "all")
        return true;
      return !current.empty() && (setting == "focused" ? output == current : setting == current);
    }
  };
} // namespace island
