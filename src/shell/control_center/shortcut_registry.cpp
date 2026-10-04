#include "shell/control_center/shortcut_registry.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/power/power_profiles_service.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/control_center/plugin_shortcut.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/keyboard_layout_label.h"
#include "shell/panel/panel_manager.h"
#include "system/gamma_service.h"
#include "system/weather_service.h"
#include "theme/theme_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <optional>
#include <ranges>
#include <vector>

std::string Shortcut::tooltipText() const {
  const std::string status = statusText();
  if (status.empty()) {
    return displayLabel();
  }
  return i18n::tr("control-center.shortcuts.tooltip", "label", defaultLabel(), "status", status);
}

namespace {
  void openTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  // ── Toggle shortcuts ────────────────────────────────────────────────────────

  class WifiShortcut final : public Shortcut {
  public:
    explicit WifiShortcut(INetworkService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "wifi"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wifi"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr) {
        const NetworkState& state = m_svc->state();
        if (state.kind == NetworkConnectivity::Wireless && state.connected && !state.ssid.empty()) {
          return state.ssid;
        }
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "wifi"; }
    std::string_view iconOff() const override { return "wifi-off"; }
    std::string displayIcon() const override {
      if (m_svc == nullptr) {
        return "wifi-question";
      }
      return network_display::wifiGlyphForState(m_svc->state());
    }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().wirelessEnabled; }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      const NetworkState& state = m_svc->state();
      if (!state.wirelessEnabled) {
        return i18n::tr("control-center.shortcuts.status.off");
      }
      if (state.kind == NetworkConnectivity::Wireless && state.connected && !state.ssid.empty()) {
        return state.ssid;
      }
      return i18n::tr("control-center.shortcuts.status.not-connected");
    }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setWirelessEnabled(!m_svc->state().wirelessEnabled);
      }
    }
    void onRightClick() override { openTab("network"); }
    bool opensDetail() const override { return true; }

  private:
    INetworkService* m_svc;
  };

  class BluetoothShortcut final : public Shortcut {
  public:
    explicit BluetoothShortcut(BluetoothService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "bluetooth"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.bluetooth"); }
    std::string_view iconOn() const override { return "bluetooth"; }
    std::string_view iconOff() const override { return "bluetooth-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().powered; }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      if (!m_svc->state().powered) {
        return i18n::tr("control-center.shortcuts.status.off");
      }
      std::string connected;
      int count = 0;
      for (const auto& device : m_svc->devices()) {
        if (!device.connected) {
          continue;
        }
        ++count;
        if (count <= 2 && !device.alias.empty()) {
          connected += connected.empty() ? device.alias : ", " + device.alias;
        }
      }
      if (count == 0) {
        return i18n::tr("control-center.shortcuts.status.on");
      }
      if (count > 2) {
        connected += std::format(" +{}", count - 2);
      }
      return connected;
    }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setPowered(!m_svc->state().powered);
      }
    }
    void onRightClick() override { openTab("bluetooth"); }
    bool opensDetail() const override { return true; }

  private:
    BluetoothService* m_svc;
  };

  class NightlightShortcut final : public Shortcut {
  public:
    NightlightShortcut(GammaService* svc, CompositorPlatform* platform) : m_svc(svc), m_platform(platform) {}
    std::string_view id() const override { return "nightlight"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.nightlight"); }
    bool enabled() const override { return m_platform != nullptr && m_platform->hasGammaControl(); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      if (m_svc->forceEnabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.forced");
      }
      if (!m_svc->enabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.off");
      }
      // Scheduled and currently warming the screen.
      if (m_svc->active()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-night");
      }
      // Scheduled but in the day phase: surface that the click "took" even
      // though the service is in day phase.
      return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-day");
    }
    std::string_view iconOn() const override {
      return m_svc != nullptr && m_svc->forceEnabled() ? "nightlight-forced" : "nightlight-on";
    }
    std::string_view iconOff() const override { return "nightlight-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && (m_svc->forceEnabled() || m_svc->active()); }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      if (!m_svc->forceEnabled() && !m_svc->enabled()) {
        return i18n::tr("control-center.shortcuts.status.off");
      }
      return displayLabel();
    }
    void onClick() override {
      if (!enabled() || m_svc == nullptr) {
        return;
      }
      m_svc->toggleEnabled();
    }
    void onRightClick() override {
      if (enabled() && m_svc != nullptr) {
        m_svc->toggleForceEnabled();
      }
    }

  private:
    GammaService* m_svc;
    CompositorPlatform* m_platform;
  };

  class NotificationShortcut final : public Shortcut {
  public:
    explicit NotificationShortcut(NotificationManager* svc) : m_svc(svc) {}
    std::string_view id() const override { return "notification"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.notification"); }
    // Focus's crescent, as macOS draws Do Not Disturb.
    std::string_view iconOn() const override { return "moon"; }
    std::string_view iconOff() const override { return "moon"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->doNotDisturb(); }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      return i18n::tr(
          m_svc->doNotDisturb() ? "control-center.shortcuts.status.on" : "control-center.shortcuts.status.off"
      );
    }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->toggleDoNotDisturb();
      }
    }
    void onRightClick() override { openTab("notifications"); }
    bool opensDetail() const override { return true; }

  private:
    NotificationManager* m_svc;
  };

  class DarkModeShortcut final : public Shortcut {
  public:
    explicit DarkModeShortcut(noctalia::theme::ThemeService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "dark_mode"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.dark-mode.dark"); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      switch (m_svc->configuredMode()) {
      case ThemeMode::Dark:
        return i18n::tr("control-center.shortcuts.dark-mode.dark");
      case ThemeMode::Light:
        return i18n::tr("control-center.shortcuts.dark-mode.light");
      case ThemeMode::Auto:
        return i18n::tr("control-center.shortcuts.dark-mode.auto");
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "theme-mode"; }
    std::string_view iconOff() const override { return "theme-mode"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->configuredMode() != ThemeMode::Light; }
    std::string tooltipText() const override {
      if (m_svc == nullptr) {
        return displayLabel();
      }
      std::string_view mode = "control-center.shortcuts.status.dark";
      switch (m_svc->configuredMode()) {
      case ThemeMode::Dark:
        break;
      case ThemeMode::Light:
        mode = "control-center.shortcuts.status.light";
        break;
      case ThemeMode::Auto:
        mode = "control-center.shortcuts.status.auto";
        break;
      }
      return i18n::tr(
          "control-center.shortcuts.tooltip", "label", i18n::tr("control-center.shortcuts.appearance"), "status",
          i18n::tr(mode)
      );
    }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->cycleMode();
      }
    }

  private:
    noctalia::theme::ThemeService* m_svc;
  };

  class IdleInhibitorShortcut final : public Shortcut {
  public:
    explicit IdleInhibitorShortcut(IdleInhibitor* svc) : m_svc(svc) {}
    std::string_view id() const override { return "caffeine"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.caffeine"); }
    std::string_view iconOn() const override { return "caffeine-on"; }
    std::string_view iconOff() const override { return "caffeine-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->enabled(); }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      return i18n::tr(m_svc->enabled() ? "control-center.shortcuts.status.on" : "control-center.shortcuts.status.off");
    }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->toggle();
      }
    }

  private:
    IdleInhibitor* m_svc;
  };

  // "Muted" or the level as a percentage, for the audio and microphone tooltips.
  std::string volumeStatus(const AudioNode& node) {
    if (node.muted) {
      return i18n::tr("control-center.shortcuts.status.muted");
    }
    return std::format("{}%", static_cast<int>(std::lround(std::clamp(node.volume, 0.0F, 1.5F) * 100.0F)));
  }

  class AudioShortcut final : public Shortcut {
  public:
    explicit AudioShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "audio"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.audio"); }
    std::string_view iconOn() const override { return "volume-x"; }
    std::string_view iconOff() const override { return "volume-high"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* sink = m_svc->defaultSink();
      return sink != nullptr && sink->muted;
    }
    std::string statusText() const override {
      const AudioNode* sink = m_svc != nullptr ? m_svc->defaultSink() : nullptr;
      if (sink == nullptr) {
        return {};
      }
      return volumeStatus(*sink);
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* sink = m_svc->defaultSink(); sink != nullptr) {
          m_svc->setMuted(!sink->muted);
        }
      }
    }
    void onRightClick() override { openTab("audio"); }
    bool opensDetail() const override { return true; }

  private:
    PipeWireService* m_svc;
  };

  class MicMuteShortcut final : public Shortcut {
  public:
    explicit MicMuteShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "mic_mute"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.mic-mute"); }
    std::string_view iconOn() const override { return "microphone-mute"; }
    std::string_view iconOff() const override { return "microphone"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* source = m_svc->defaultSource();
      return source != nullptr && source->muted;
    }
    std::string statusText() const override {
      const AudioNode* source = m_svc != nullptr ? m_svc->defaultSource() : nullptr;
      if (source == nullptr) {
        return {};
      }
      return volumeStatus(*source);
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* source = m_svc->defaultSource(); source != nullptr) {
          m_svc->setMicMuted(!source->muted);
        }
      }
    }
    void onRightClick() override { openTab("audio"); }
    bool opensDetail() const override { return true; }

  private:
    PipeWireService* m_svc;
  };

  class PowerProfileShortcut final : public Shortcut {
  public:
    explicit PowerProfileShortcut(PowerProfilesService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "power_profile"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.power-profile"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && !m_svc->activeProfile().empty()) {
        return profileLabel(m_svc->activeProfile());
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override {
      return profileGlyphName(m_svc != nullptr ? m_svc->activeProfile() : "");
    }
    std::string_view iconOff() const override { return "balanced"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      return m_svc != nullptr && !m_svc->activeProfile().empty() && m_svc->activeProfile() != "balanced";
    }
    std::string statusText() const override {
      if (m_svc == nullptr || m_svc->activeProfile().empty()) {
        return {};
      }
      return profileLabel(m_svc->activeProfile());
    }
    void onClick() override { cycle(1); }
    void onRightClick() override { cycle(-1); }
    void onScroll(int direction) override { cycle(direction); }

  private:
    void cycle(int direction) {
      if (m_svc != nullptr) {
        (void)m_svc->cycleActiveProfile(direction);
      }
    }

    PowerProfilesService* m_svc;
  };

  class WeatherShortcut final : public Shortcut {
  public:
    explicit WeatherShortcut(WeatherService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "weather"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.weather"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && m_svc->enabled() && m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        const int temp = static_cast<int>(std::lround(m_svc->displayTemperature(snapshot.current.temperatureC)));
        return std::format("{}{}", temp, m_svc->displayTemperatureUnit());
      }
      return defaultLabel();
    }
    std::string displayIcon() const override {
      if (m_svc == nullptr || !m_svc->enabled()) {
        return "weather-cloud-off";
      }
      if (m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        return WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
      }
      return "weather-cloud";
    }
    std::string statusText() const override {
      if (m_svc == nullptr || !m_svc->enabled()) {
        return m_svc == nullptr ? std::string{} : i18n::tr("control-center.shortcuts.status.off");
      }
      if (!m_svc->hasData()) {
        return {};
      }
      const auto& snapshot = m_svc->snapshot();
      std::string status =
          std::format("{} {}", displayLabel(), WeatherService::shortDescriptionForCode(snapshot.current.weatherCode));
      if (!snapshot.locationName.empty()) {
        status += " · " + snapshot.locationName;
      }
      return status;
    }
    std::string_view iconOn() const override { return "weather-cloud-sun"; }
    std::string_view iconOff() const override { return "weather-cloud-sun"; }
    void onClick() override { openTab("weather"); }
    void onRightClick() override { openTab("weather"); }
    bool opensDetail() const override { return true; }

  private:
    WeatherService* m_svc;
  };

  class KeyboardLayoutShortcut final : public Shortcut {
  public:
    KeyboardLayoutShortcut(CompositorPlatform* platform, ConfigService* config)
        : m_platform(platform), m_config(config) {}
    std::string_view id() const override { return "keyboard_layout"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.keyboard-layout"); }
    std::string displayLabel() const override {
      const std::string layoutName = resolvedLayoutName();
      if (m_config == nullptr) {
        return formatKeyboardLayoutLabel(layoutName, KeyboardLayoutDisplayMode::Short);
      }
      return resolveKeyboardLayoutLabel(
          layoutName, KeyboardLayoutDisplayMode::Short, m_config->config().shell.keyboardLayout.customLabels
      );
    }
    std::string statusText() const override {
      const std::string layoutName = resolvedLayoutName();
      if (layoutName.empty()) {
        return {};
      }
      if (m_config == nullptr) {
        return formatKeyboardLayoutLabel(layoutName, KeyboardLayoutDisplayMode::Full);
      }
      return resolveKeyboardLayoutLabel(
          layoutName, KeyboardLayoutDisplayMode::Full, m_config->config().shell.keyboardLayout.customLabels
      );
    }
    std::string_view iconOn() const override { return "keyboard"; }
    std::string_view iconOff() const override { return "keyboard"; }
    void onClick() override {
      if (m_platform != nullptr) {
        (void)m_platform->cycleKeyboardLayout();
      }
      PanelManager::instance().refresh();
    }

  private:
    [[nodiscard]] std::string resolvedLayoutName() const {
      const auto state = m_platform != nullptr ? m_platform->keyboardLayoutState() : std::nullopt;
      if (state.has_value()
          && state->currentIndex >= 0
          && state->currentIndex < static_cast<int>(state->names.size())) {
        return state->names[static_cast<std::size_t>(state->currentIndex)];
      }

      if (m_platform != nullptr) {
        return m_platform->currentKeyboardLayoutName();
      }

      return {};
    }

    CompositorPlatform* m_platform = nullptr;
    ConfigService* m_config = nullptr;
  };

  // ── Action-only shortcuts ────��──────────────────────────────────────────────

  class MediaShortcut final : public Shortcut {
  public:
    explicit MediaShortcut(MprisService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "media"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.media"); }
    std::string_view iconOn() const override { return "media-pause"; }
    std::string_view iconOff() const override { return "media-play"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const auto active = m_svc->activePlayer();
      return active.has_value() && active->playbackStatus == "Playing";
    }
    std::string statusText() const override {
      if (m_svc == nullptr) {
        return {};
      }
      const auto player = m_svc->activePlayer();
      if (!player.has_value()) {
        return i18n::tr("control-center.shortcuts.status.nothing-playing");
      }
      std::string track = player->title;
      if (!player->artists.empty() && !player->artists.front().empty()) {
        track += track.empty() ? player->artists.front() : " – " + player->artists.front();
      }
      if (track.empty()) {
        track = player->identity;
      }
      const bool playing = player->playbackStatus == "Playing";
      const std::string state =
          i18n::tr(playing ? "control-center.shortcuts.status.playing" : "control-center.shortcuts.status.paused");
      return track.empty() ? state : std::format("{} · {}", state, track);
    }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->playPauseActive();
      }
    }
    void onRightClick() override { openTab("media"); }
    bool opensDetail() const override { return true; }

  private:
    MprisService* m_svc;
  };

  class SystemShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "system"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.system"); }
    std::string_view iconOn() const override { return "activity"; }
    std::string_view iconOff() const override { return "activity"; }
    void onClick() override { openTab("system"); }
    void onRightClick() override { openTab("system"); }
    bool opensDetail() const override { return true; }
  };

  class ScreenTimeShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "screen_time"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screen-time"); }
    std::string_view iconOn() const override { return "hourglass"; }
    std::string_view iconOff() const override { return "hourglass"; }
    void onClick() override { openTab("screen-time"); }
    void onRightClick() override { openTab("screen-time"); }
    bool opensDetail() const override { return true; }
  };

  class WallpaperShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "wallpaper"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wallpaper"); }
    std::string_view iconOn() const override { return "wallpaper-selector"; }
    std::string_view iconOff() const override { return "wallpaper-selector"; }
    void onClick() override { PanelManager::instance().togglePanel("wallpaper"); }
  };

  class SessionShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "session"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.session"); }
    std::string_view iconOn() const override { return "shutdown"; }
    std::string_view iconOff() const override { return "shutdown"; }
    void onClick() override { PanelManager::instance().togglePanel("session"); }
  };

  class ClipboardShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "clipboard"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.clipboard"); }
    std::string_view iconOn() const override { return "clipboard"; }
    std::string_view iconOff() const override { return "clipboard"; }
    void onClick() override { PanelManager::instance().togglePanel("clipboard"); }
  };

  using ShortcutCreator = std::unique_ptr<Shortcut> (*)(const ShortcutServices& services);
  using ShortcutAvailability = bool (*)(const Config& config);

  struct BuiltinShortcutDescriptor {
    std::string_view type;
    std::string_view labelKey;
    ShortcutAvailability isAvailable = nullptr;
    ShortcutCreator create = nullptr;
  };

  template <typename T, auto... ServiceMembers>
  constexpr BuiltinShortcutDescriptor builtinShortcut(BuiltinShortcutDescriptor descriptor) {
    descriptor.create = [](const ShortcutServices& services) -> std::unique_ptr<Shortcut> {
      return std::make_unique<T>((services.*ServiceMembers)...);
    };
    return descriptor;
  }

  constexpr auto kBuiltinShortcuts = std::to_array<BuiltinShortcutDescriptor>({
      builtinShortcut<WifiShortcut, &ShortcutServices::network>({
          .type = "wifi",
          .labelKey = "control-center.shortcuts.wifi",
      }),
      builtinShortcut<BluetoothShortcut, &ShortcutServices::bluetooth>({
          .type = "bluetooth",
          .labelKey = "control-center.shortcuts.bluetooth",
      }),
      builtinShortcut<NightlightShortcut, &ShortcutServices::nightLight, &ShortcutServices::platform>({
          .type = "nightlight",
          .labelKey = "control-center.shortcuts.nightlight",
      }),
      builtinShortcut<NotificationShortcut, &ShortcutServices::notifications>({
          .type = "notification",
          .labelKey = "control-center.shortcuts.notification",
      }),
      builtinShortcut<DarkModeShortcut, &ShortcutServices::theme>({
          .type = "dark_mode",
          .labelKey = "control-center.shortcuts.dark-mode.dark",
      }),
      builtinShortcut<IdleInhibitorShortcut, &ShortcutServices::idleInhibitor>({
          .type = "caffeine",
          .labelKey = "control-center.shortcuts.caffeine",
      }),
      builtinShortcut<AudioShortcut, &ShortcutServices::audio>({
          .type = "audio",
          .labelKey = "control-center.shortcuts.audio",
      }),
      builtinShortcut<MicMuteShortcut, &ShortcutServices::audio>({
          .type = "mic_mute",
          .labelKey = "control-center.shortcuts.mic-mute",
      }),
      builtinShortcut<PowerProfileShortcut, &ShortcutServices::powerProfiles>({
          .type = "power_profile",
          .labelKey = "control-center.shortcuts.power-profile",
      }),
      builtinShortcut<MediaShortcut, &ShortcutServices::mpris>({
          .type = "media",
          .labelKey = "control-center.shortcuts.media",
      }),
      builtinShortcut<WeatherShortcut, &ShortcutServices::weather>({
          .type = "weather",
          .labelKey = "control-center.shortcuts.weather",
          .isAvailable = [](const Config& config) { return config.weather.enabled; },
      }),
      builtinShortcut<SystemShortcut>({
          .type = "system",
          .labelKey = "control-center.shortcuts.system",
          .isAvailable = [](const Config& config) { return config.system.monitor.enabled; },
      }),
      builtinShortcut<ScreenTimeShortcut>({
          .type = "screen_time",
          .labelKey = "control-center.shortcuts.screen-time",
          .isAvailable = [](const Config& config) { return config.shell.screenTimeEnabled; },
      }),
      builtinShortcut<KeyboardLayoutShortcut, &ShortcutServices::platform, &ShortcutServices::config>({
          .type = "keyboard_layout",
          .labelKey = "control-center.shortcuts.keyboard-layout",
      }),
      builtinShortcut<WallpaperShortcut>({
          .type = "wallpaper",
          .labelKey = "control-center.shortcuts.wallpaper",
      }),
      builtinShortcut<SessionShortcut>({
          .type = "session",
          .labelKey = "control-center.shortcuts.session",
      }),
      builtinShortcut<ClipboardShortcut>({
          .type = "clipboard",
          .labelKey = "control-center.shortcuts.clipboard",
          .isAvailable = [](const Config& config) { return config.shell.clipboardEnabled; },
      }),
  });

  const BuiltinShortcutDescriptor* findBuiltinShortcut(std::string_view type) {
    for (const auto& shortcut : kBuiltinShortcuts) {
      if (shortcut.type == type) {
        return &shortcut;
      }
    }
    return nullptr;
  }

} // namespace

std::span<const ShortcutRegistry::CatalogEntry> ShortcutRegistry::catalog() {
  // Built-in shortcuts plus every plugin [[shortcut]] entry. Plugin id/label
  // strings are held in a stable static deque so the CatalogEntry views stay valid.
  static std::deque<std::string> storage;
  static const std::vector<CatalogEntry> combined = [] {
    auto result = kBuiltinShortcuts
        | std::views::transform([](const BuiltinShortcutDescriptor& shortcut) {
                    return CatalogEntry{.type = shortcut.type, .labelKey = shortcut.labelKey};
                  })
        | std::ranges::to<std::vector>();
    scripting::PluginRegistry::instance().ensureScanned();
    for (const auto& entry :
         scripting::PluginRegistry::instance().entriesOfKind(scripting::PluginEntryKind::Shortcut)) {
      storage.push_back(entry.fullId());
      const std::string_view typeView = storage.back();
      storage.push_back(entry.manifest->name.empty() ? entry.fullId() : entry.manifest->name);
      const std::string_view labelView = storage.back();
      result.push_back(CatalogEntry{.type = typeView, .labelKey = labelView, .literalLabel = true});
    }
    return result;
  }();
  return combined;
}

bool ShortcutRegistry::isAvailable(std::string_view type, const Config& config) {
  const auto* shortcut = findBuiltinShortcut(type);
  return shortcut == nullptr || shortcut->isAvailable == nullptr || shortcut->isAvailable(config);
}

std::unique_ptr<Shortcut> ShortcutRegistry::create(std::string_view type, const ShortcutServices& s) {
  if (auto entry = scripting::PluginRegistry::instance().resolve(type);
      entry.has_value() && entry->entry->kind == scripting::PluginEntryKind::Shortcut) {
    if (s.scriptApi == nullptr) {
      return nullptr;
    }
    auto seeded = scripting::seedEntrySettings(*entry->entry, {});
    static const std::unordered_map<std::string, WidgetSettingValue> kNoPluginOverrides;
    const auto* overrides = &kNoPluginOverrides;
    if (s.config != nullptr) {
      const auto& pluginSettings = s.config->config().plugins.pluginSettings;
      if (const auto psIt = pluginSettings.find(entry->manifest->id); psIt != pluginSettings.end()) {
        overrides = &psIt->second;
      }
    }
    scripting::mergePluginSettings(*entry->manifest, *overrides, seeded);
    return std::make_unique<PluginShortcut>(scripting::PluginRuntimeContext{
        .entryId = entry->fullId(),
        .sourcePath = entry->sourcePath,
        .pluginDir = entry->pluginDir,
        .settings = std::move(seeded),
        .scriptApi = *s.scriptApi,
        .fileWatcher = s.fileWatcher,
        .httpClient = s.httpClient,
        .clipboard = s.clipboard,
        .platform = s.platform,
    });
  }
  const auto* shortcut = findBuiltinShortcut(type);
  if (shortcut == nullptr || (s.config != nullptr && !isAvailable(type, s.config->config()))) {
    return nullptr;
  }
  return shortcut->create(s);
}
