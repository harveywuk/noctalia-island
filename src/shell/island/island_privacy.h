#pragma once

#include "config/config_types.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/privacy_filter.h"

#include <algorithm>
#include <array>

namespace island {
  struct PrivacyActivity {
    PrivacyCaptureKind kind;
    std::vector<std::string> apps;
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
        if (index < groups.size() && !capture.appName.empty() && !m_filters[index].matches(capture.appName))
          groups[index].apps.push_back(capture.appName);
      }
      std::vector<PrivacyActivity> result;
      for (auto& group : groups) {
        std::ranges::sort(group.apps);
        const auto duplicates = std::ranges::unique(group.apps);
        group.apps.erase(duplicates.begin(), duplicates.end());
        if (!group.apps.empty()) result.push_back(std::move(group));
      }
      return result;
    }
  private:
    std::array<PrivacyFilter, 3> m_filters;
  };
} // namespace island
