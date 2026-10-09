#pragma once

#include "config/config_types.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/privacy_filter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>

namespace island {
  // Per-app elapsed time starts when an active, unfiltered capture is first observed.
  // Parallel streams from the same app share a session until its last stream ends.
  class CaptureSessions {
  public:
    using Clock = std::chrono::steady_clock;
    void update(const std::vector<std::string>& apps, Clock::time_point now) {
      std::erase_if(m_started, [&](const auto& entry) { return !std::ranges::contains(apps, entry.first); });
      for (const auto& app : apps)
        m_started.try_emplace(app, now);
    }
    const auto& sessions() const { return m_started; }
    std::optional<std::chrono::seconds> elapsed(const std::string& app, Clock::time_point now) const {
      const auto found = m_started.find(app);
      if (found == m_started.end())
        return std::nullopt;
      return std::max(
          std::chrono::seconds::zero(), std::chrono::duration_cast<std::chrono::seconds>(now - found->second)
      );
    }

  private:
    std::map<std::string, Clock::time_point> m_started;
  };

  struct PrivacyActivity {
    PrivacyCaptureKind kind;
    std::vector<std::string> apps;
    // Lower-case executables of those apps, where known, for focusing their windows.
    std::vector<std::string> binaries;
    std::vector<std::uint32_t> sourceIds;
    const char* icon() const {
      switch (kind) {
      case PrivacyCaptureKind::Microphone:
        return "privacy-microphone";
      case PrivacyCaptureKind::Camera:
        return "privacy-camera";
      case PrivacyCaptureKind::Screen:
        return "privacy-screen";
      }
      return "shield";
    }
    const char* labelKey() const {
      switch (kind) {
      case PrivacyCaptureKind::Microphone:
        return "bar.widgets.privacy.microphone";
      case PrivacyCaptureKind::Camera:
        return "bar.widgets.privacy.camera";
      case PrivacyCaptureKind::Screen:
        return "bar.widgets.privacy.screen-sharing";
      }
      return "";
    }
    std::string appNames() const {
      std::string text;
      for (const auto& app : apps) {
        if (!text.empty())
          text += ", ";
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
      std::array<PrivacyActivity, 3> groups{
          {{PrivacyCaptureKind::Microphone, {}}, {PrivacyCaptureKind::Camera, {}}, {PrivacyCaptureKind::Screen, {}}}
      };
      for (const auto& capture : state.captures) {
        const auto index = static_cast<std::size_t>(capture.kind);
        if (index < groups.size() && !capture.appName.empty() && !m_filters[index].matches(capture.appName)) {
          groups[index].apps.push_back(capture.appName);
          groups[index].sourceIds.insert(
              groups[index].sourceIds.end(), capture.sourceIds.begin(), capture.sourceIds.end()
          );
          if (!capture.binary.empty())
            groups[index].binaries.push_back(capture.binary);
        }
      }
      std::vector<PrivacyActivity> result;
      for (auto& group : groups) {
        std::ranges::sort(group.sourceIds);
        const auto duplicateSources = std::ranges::unique(group.sourceIds);
        group.sourceIds.erase(duplicateSources.begin(), duplicateSources.end());
        std::ranges::sort(group.apps);
        const auto duplicates = std::ranges::unique(group.apps);
        group.apps.erase(duplicates.begin(), duplicates.end());
        std::ranges::sort(group.binaries);
        const auto duplicateBinaries = std::ranges::unique(group.binaries);
        group.binaries.erase(duplicateBinaries.begin(), duplicateBinaries.end());
        if (!group.apps.empty())
          result.push_back(std::move(group));
      }
      return result;
    }

  private:
    std::array<PrivacyFilter, 3> m_filters;
  };

  // Compact views give their indicators (capture kinds, unread notifications) a single slot,
  // cycling through whichever are active.
  class PrivacyRotation {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kInterval = std::chrono::seconds(5);

    // Index into `ids` of the indicator to show now. A newly active one is shown at once;
    // `hold` (the pointer is on the slot) keeps the current one so its tooltip and click target
    // stay put.
    std::optional<std::size_t> pick(const std::vector<std::string>& ids, Clock::time_point now, bool hold = false) {
      if (ids.empty()) {
        m_shown.clear();
        m_ids.clear();
        return std::nullopt;
      }
      const auto index = [&](const std::string& id) {
        return static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
      };
      std::optional<std::string> added;
      for (const auto& id : ids)
        if (!std::ranges::contains(m_ids, id))
          added = id;
      m_ids = ids;
      if (added && !hold)
        show(*added, now);
      else if (index(m_shown) == ids.size())
        show(ids.front(), now);
      else if (hold)
        m_shownAt = now;
      else if (now - m_shownAt >= kInterval)
        show(ids[(index(m_shown) + 1) % ids.size()], now);
      return index(m_shown);
    }

  private:
    void show(const std::string& id, Clock::time_point now) {
      m_shown = id;
      m_shownAt = now;
    }

    std::string m_shown;
    Clock::time_point m_shownAt;
    std::vector<std::string> m_ids;
  };
} // namespace island
