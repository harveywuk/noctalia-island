#pragma once

#include "config/config_types.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/privacy_filter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>

namespace island {
  struct PrivacyActivity {
    PrivacyCaptureKind kind;
    std::vector<std::string> apps;
    // Lower-case executables of those apps, where known, for focusing their windows.
    std::vector<std::string> binaries;
    const char* icon() const {
      switch (kind) {
      case PrivacyCaptureKind::Microphone: return "microphone";
      case PrivacyCaptureKind::Camera: return "camera";
      case PrivacyCaptureKind::Screen: return "screen-share";
      }
      return "shield";
    }
    const char* labelKey() const {
      switch (kind) {
      case PrivacyCaptureKind::Microphone: return "bar.widgets.privacy.microphone";
      case PrivacyCaptureKind::Camera: return "bar.widgets.privacy.camera";
      case PrivacyCaptureKind::Screen: return "bar.widgets.privacy.screen-sharing";
      }
      return "";
    }
    std::string appNames() const {
      std::string text;
      for (const auto& app : apps) {
        if (!text.empty()) text += ", ";
        text += app;
      }
      return text;
    }
  };

  class PrivacySummary {
  public:
    std::vector<PrivacyActivity> snapshot(const PrivacyState& state, const ShellConfig::PrivacyConfig& config) {
      m_filters[0].update("shell.privacy.mic_filter_regex", config.micFilterRegex);
      m_filters[1].update("shell.privacy.cam_filter_regex", config.camFilterRegex);
      m_filters[2].update("shell.privacy.screen_filter_regex", config.screenFilterRegex);
      std::array<PrivacyActivity, 3> groups{{{PrivacyCaptureKind::Microphone, {}},
          {PrivacyCaptureKind::Camera, {}}, {PrivacyCaptureKind::Screen, {}}}};
      for (const auto& capture : state.captures) {
        const auto index = static_cast<std::size_t>(capture.kind);
        if (index < groups.size() && !capture.appName.empty() && !m_filters[index].matches(capture.appName)) {
          groups[index].apps.push_back(capture.appName);
          if (!capture.binary.empty())
            groups[index].binaries.push_back(capture.binary);
        }
      }
      std::vector<PrivacyActivity> result;
      for (auto& group : groups) {
        std::ranges::sort(group.apps);
        const auto duplicates = std::ranges::unique(group.apps);
        group.apps.erase(duplicates.begin(), duplicates.end());
        std::ranges::sort(group.binaries);
        const auto duplicateBinaries = std::ranges::unique(group.binaries);
        group.binaries.erase(duplicateBinaries.begin(), duplicateBinaries.end());
        if (!group.apps.empty()) result.push_back(std::move(group));
      }
      return result;
    }
  private:
    std::array<PrivacyFilter, 3> m_filters;
  };

  // Compact views give capture indicators a single slot, cycling through the active kinds.
  class PrivacyRotation {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kInterval = std::chrono::seconds(5);

    // The activity to show now. A newly started capture is shown at once; `hold` (the pointer
    // is on the icon) keeps the current one so its tooltip and click target stay put.
    const PrivacyActivity* pick(const std::vector<PrivacyActivity>& list, Clock::time_point now, bool hold = false) {
      if (list.empty()) {
        m_shown.reset();
        m_kinds.clear();
        return nullptr;
      }
      std::vector<PrivacyCaptureKind> kinds;
      for (const auto& activity : list)
        kinds.push_back(activity.kind);
      const auto index = [&](PrivacyCaptureKind kind) {
        return static_cast<std::size_t>(std::ranges::find(kinds, kind) - kinds.begin());
      };
      std::optional<PrivacyCaptureKind> added;
      for (const auto kind : kinds)
        if (!std::ranges::contains(m_kinds, kind))
          added = kind;
      m_kinds = kinds;
      if (added && !hold) {
        show(*added, now);
      } else if (!m_shown || index(*m_shown) == kinds.size()) {
        show(kinds.front(), now);
      } else if (hold) {
        m_shownAt = now;
      } else if (now - m_shownAt >= kInterval) {
        show(kinds[(index(*m_shown) + 1) % kinds.size()], now);
      }
      return &list[index(*m_shown)];
    }

  private:
    void show(PrivacyCaptureKind kind, Clock::time_point now) {
      m_shown = kind;
      m_shownAt = now;
    }

    std::optional<PrivacyCaptureKind> m_shown;
    Clock::time_point m_shownAt;
    std::vector<PrivacyCaptureKind> m_kinds;
  };
} // namespace island
