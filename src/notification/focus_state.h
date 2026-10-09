#pragma once

#include "config/config_types.h"
#include "util/string_utils.h"

#include <array>
#include <cctype>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace focus {
  // Temporary policy only. The underlying manual choice and schedule keep running.
  // A user choice takes precedence until the next recording, even across reloads.
  class RecordingOverride {
  public:
    void configure(bool enabled) { m_enabled = enabled; }
    void setRecording(bool recording) {
      if (m_recording == recording)
        return;
      m_recording = recording;
      m_overridden = false;
    }
    void userChangedFocus() {
      if (m_recording)
        m_overridden = true;
    }
    bool active() const { return m_enabled && m_recording && !m_overridden; }

  private:
    bool m_enabled = false;
    bool m_recording = false;
    bool m_overridden = false;
  };

  inline constexpr std::array<std::string_view, 3> kProfiles{"work", "gaming", "sleep"};

  inline const FocusProfileConfig* profile(const FocusConfig& config, std::string_view id) {
    if (id == "work")
      return &config.work;
    if (id == "gaming")
      return &config.gaming;
    if (id == "sleep")
      return &config.sleep;
    return nullptr;
  }

  inline bool scheduled(const FocusProfileConfig& profile, const std::tm& local) {
    if (!profile.scheduleEnabled || profile.startMinute == profile.endMinute)
      return false;
    const int minute = local.tm_hour * 60 + local.tm_min;
    int day = (local.tm_wday + 6) % 7;
    if (profile.startMinute < profile.endMinute) {
      if (minute < profile.startMinute || minute >= profile.endMinute)
        return false;
    } else {
      if (minute < profile.endMinute)
        day = (day + 6) % 7;
      else if (minute < profile.startMinute)
        return false;
    }
    return (profile.days & (1 << day)) != 0;
  }

  inline std::string identity(std::string_view value) {
    auto result = StringUtils::trim(std::string(value));
    for (char& c : result)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (result.ends_with(".desktop"))
      result.resize(result.size() - 8);
    return result;
  }

  inline bool allowsApp(const FocusProfileConfig& profile, std::string_view app, std::string_view desktop) {
    const auto name = identity(app), id = identity(desktop);
    for (const auto& allowed : profile.allowedApps) {
      const auto candidate = identity(allowed);
      if (!candidate.empty() && (candidate == name || candidate == id))
        return true;
    }
    return false;
  }

  // A manual choice lasts until the scheduled profile changes. Off therefore suppresses the
  // current scheduled interval without disabling tomorrow's schedule. No notification is replayed.
  class State {
  public:
    void configure(FocusConfig config) { m_config = std::move(config); }
    void update(const std::tm& local) {
      std::string scheduledId;
      // Deterministic overlap priority: Sleep, Gaming, Work.
      for (auto id : kProfiles)
        if (scheduled(*profile(m_config, id), local))
          scheduledId = id;
      if (m_scheduleKnown && scheduledId != m_scheduled)
        m_manual.reset();
      m_scheduleKnown = true;
      m_scheduled = std::move(scheduledId);
      m_active = m_manual.value_or(m_scheduled);
    }
    bool select(std::string_view id, const std::tm& local) {
      if (id != "off" && id != "auto" && !profile(m_config, id))
        return false;
      update(local);
      if (id == "auto")
        m_manual.reset();
      else
        m_manual = id == "off" ? std::string{} : std::string(id);
      m_active = m_manual.value_or(m_scheduled);
      return true;
    }
    const std::string& active() const { return m_active; }
    bool automatic() const { return !m_manual.has_value(); }
    const FocusProfileConfig* current() const { return profile(m_config, m_active); }

  private:
    FocusConfig m_config;
    std::optional<std::string> m_manual;
    std::string m_scheduled, m_active;
    bool m_scheduleKnown = false;
  };
} // namespace focus
