#pragma once

#include "dbus/bluetooth/bluetooth_glyphs.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/island/island_preview_target.h"
#include "shell/island/island_state.h"
#include "system/battery_warning_monitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace island {
  inline std::string batteryAddress(std::string address) {
    std::ranges::transform(address, address.begin(), [](unsigned char c) { return std::tolower(c); });
    return address;
  }

  // Key connection age by BlueZ identity, independently of when battery data arrives.
  class BatteryConnections {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kGlowSeconds = 10;
    void update(
        const std::vector<BluetoothDeviceInfo>& devices, Clock::time_point now, std::string_view focusedOutput = {}
    ) {
      std::erase_if(m_connected, [&](const auto& entry) {
        return std::ranges::none_of(devices, [&](const auto& device) {
          return device.connected && device.path == entry.first;
        });
      });
      for (const auto& device : devices)
        if (device.connected) {
          const auto [entry, added] =
              m_connected.try_emplace(device.path, Connection{now, PreviewTarget{std::string(focusedOutput)}});
          if (added)
            m_latest = Event{device.path, true, entry->second};
        }
    }
    void updatePower(
        const std::vector<UPowerDeviceInfo>& devices, Clock::time_point now, std::string_view focusedOutput = {},
        const std::vector<BluetoothDeviceInfo>& bluetooth = {}
    ) {
      std::erase_if(m_power, [&](const auto& entry) {
        return std::ranges::none_of(devices, [&](const auto& device) {
          return device.isPresent && device.path == entry.first;
        });
      });
      for (const auto& device : devices) {
        if (!device.isPresent)
          continue;
        const auto plugged = batteryStatePlugged(device.state.state);
        auto [entry, added] = m_power.try_emplace(device.path);
        auto& connection = entry->second;
        const bool system = device.isLaptopBattery() || device.type == UPowerDeviceType::Ups;
        if ((added && !system) || (plugged.value_or(false) && !connection.plugged)) {
          connection.preview = Connection{now, PreviewTarget{std::string(focusedOutput)}};
          // BlueZ owns a shared wireless device's event age; late UPower data is not a new connection.
          const bool wireless = !device.serial.empty() && std::ranges::any_of(bluetooth, [&](const auto& bt) {
            return batteryAddress(bt.address) == batteryAddress(device.serial);
          });
          if (!system && !wireless)
            m_latest = Event{device.path, false, *connection.preview};
        } else if (plugged == false && connection.plugged)
          connection.preview.reset();
        // A temporarily unknown state must not manufacture another charger connection.
        if (plugged)
          connection.plugged = *plugged;
      }
    }
    void reconcileOutputs(const std::vector<std::string>& available, std::string_view focused) {
      for (auto& [path, connection] : m_connected)
        connection.target.reconcile(available, focused);
      for (auto& [path, connection] : m_power)
        if (connection.preview)
          connection.preview->target.reconcile(available, focused);
      if (m_latest)
        m_latest->connection.target.reconcile(available, focused);
    }
    struct Preview {
      std::string path;
      bool bluetooth;
      Clock::time_point started;
    };
    std::optional<Preview>
    preview(Clock::time_point now, int seconds, std::string_view setting = "all", std::string_view output = {}) const {
      // Retain the latest identity after removal/expiry so older connections never replay.
      if (!m_latest
          || now >= m_latest->connection.started + std::chrono::seconds(seconds)
          || !m_latest->connection.target.matches(setting, output))
        return std::nullopt;
      const auto active = started(m_latest->path, now, seconds, setting, output);
      return active == m_latest->connection.started
          ? std::optional{Preview{m_latest->path, m_latest->bluetooth, *active}}
          : std::nullopt;
    }
    // Opening the card consumes its preview on every monitor, leaving the glow's age intact.
    void dismissPreview() { m_latest.reset(); }
    std::optional<Clock::time_point> started(
        const std::string& path, Clock::time_point now, int seconds, std::string_view setting, std::string_view output
    ) const {
      const Connection* connection = nullptr;
      if (const auto found = m_connected.find(path); found != m_connected.end())
        connection = &found->second;
      else if (const auto power = m_power.find(path); power != m_power.end() && power->second.preview)
        connection = &*power->second.preview;
      if (connection
          && now < connection->started + std::chrono::seconds(seconds)
          && connection->target.matches(setting, output))
        return connection->started;
      return std::nullopt;
    }
    bool recent(
        const std::string& path, Clock::time_point now, int seconds = 5, std::string_view setting = "all",
        std::string_view output = {}
    ) const {
      return started(path, now, seconds, setting, output).has_value();
    }
    std::optional<Clock::time_point> nextExpiry(Clock::time_point now, int seconds = 5) const {
      std::optional<Clock::time_point> next;
      for (const auto& [path, connected] : m_connected) {
        const auto deadline = connected.started + std::chrono::seconds(seconds);
        if (deadline > now && (!next || deadline < *next))
          next = deadline;
      }
      for (const auto& [path, connected] : m_power) {
        if (!connected.preview)
          continue;
        const auto deadline = connected.preview->started + std::chrono::seconds(seconds);
        if (deadline > now && (!next || deadline < *next))
          next = deadline;
      }
      return next;
    }

  private:
    struct Connection {
      Clock::time_point started;
      PreviewTarget target;
    };
    std::unordered_map<std::string, Connection> m_connected;
    struct PowerConnection {
      std::optional<Connection> preview;
      bool plugged = false;
    };
    std::unordered_map<std::string, PowerConnection> m_power;
    struct Event {
      std::string path;
      bool bluetooth;
      Connection connection;
    };
    std::optional<Event> m_latest;
  };

  struct Battery {
    std::string id, name, icon;
    double percentage = 0;
    BatteryState state = BatteryState::Unknown;
    std::int64_t seconds = 0;
    bool system = false, low = false;
    bool bluetooth = false, recentlyConnected = false;
    std::optional<BatteryConnections::Clock::time_point> glowStarted;
    bool charging() const { return state == BatteryState::Charging; }
    bool compact() const { return bluetooth ? low || recentlyConnected : charging() || low || !system; }
  };

  enum class BatteryGlowLevel { Red, Amber, Green };
  inline BatteryGlowLevel batteryGlowLevel(double percentage) {
    return percentage > 60 ? BatteryGlowLevel::Green
        : percentage >= 20 ? BatteryGlowLevel::Amber
                           : BatteryGlowLevel::Red;
  }

  inline const Battery* glowingBattery(const std::vector<Battery>& batteries) {
    const Battery* latest = nullptr;
    for (const auto& battery : batteries)
      if (battery.glowStarted && (!latest || battery.glowStarted > latest->glowStarted))
        latest = &battery;
    return latest;
  }

  inline int bluetoothBatteryThreshold(const BatteryConfig& config, const UPowerDeviceInfo& device) {
    for (const auto& threshold : config.deviceThresholds)
      if (upowerDeviceMatchesSelector(device, threshold.selector))
        return threshold.warningThreshold;
    return config.warningThreshold;
  }

  // Read the existing services' snapshots; never start another poller or probe devices.
  inline std::vector<Battery> batterySnapshot(
      const std::vector<UPowerDeviceInfo>& power, const std::vector<BluetoothDeviceInfo>& bluetooth,
      const BatteryConfig& config, const UPowerDeviceInfo* systemBattery = nullptr,
      const BatteryConnections* connections = nullptr,
      BatteryConnections::Clock::time_point now = BatteryConnections::Clock::now(), int previewSeconds = 5,
      std::string_view previewMonitor = "all", std::string_view output = {}
  ) {
    std::vector<Battery> result;
    for (const auto& device : power) {
      const auto& state = device.state;
      if (!device.isPresent
          || !std::isfinite(state.percentage)
          || state.percentage < 0
          || state.percentage > 100
          || (state.percentage == 0 && state.state == BatteryState::Unknown))
        continue;
      const auto bt = std::ranges::find_if(bluetooth, [&](const auto& item) {
        return !device.serial.empty() && batteryAddress(item.address) == batteryAddress(device.serial);
      });
      if (bt != bluetooth.end() && !bt->connected)
        continue;
      const bool system = device.isLaptopBattery() || device.type == UPowerDeviceType::Ups;
      const bool wireless = bt != bluetooth.end() && !system;
      const int threshold = wireless ? bluetoothBatteryThreshold(config, device)
                                     : batteryWarningThresholdForDevice(config, device, systemBattery);
      result.push_back(
          {device.path,
           system                      ? i18n::tr("power.battery.tooltip.device")
               : !device.model.empty() ? device.model
                                       : i18n::tr("power.battery.tooltip.unknown-device"),
           system         ? "device-computer"
               : wireless ? bluetoothDeviceGlyphName(bt->kind)
                          : batteryDeviceGlyphName(device.type),
           state.percentage, state.state,
           state.state == BatteryState::Charging          ? state.timeToFull
               : state.state == BatteryState::Discharging ? state.timeToEmpty
                                                          : 0,
           system,
           threshold > 0
               && state.percentage <= threshold
               && (wireless || !batteryStatePlugged(state.state).value_or(false)),
           wireless,
           wireless && connections && connections->recent(bt->path, now, previewSeconds, previewMonitor, output),
           connections
               ? connections->started(
                     wireless ? bt->path : device.path, now, BatteryConnections::kGlowSeconds, previewMonitor, output
                 )
               : std::nullopt}
      );
    }
    for (const auto& device : bluetooth) {
      if (!device.connected || !device.hasBattery || device.batteryPercent > 100)
        continue;
      const bool duplicate = std::ranges::any_of(power, [&](const auto& item) {
        return !item.serial.empty()
            && batteryAddress(item.serial) == batteryAddress(device.address)
            && std::ranges::any_of(result, [&](const Battery& battery) { return battery.id == item.path; });
      });
      if (duplicate)
        continue;
      UPowerDeviceInfo identity;
      identity.path = device.path;
      identity.serial = device.address;
      identity.model = device.alias;
      const int threshold = bluetoothBatteryThreshold(config, identity);
      result.push_back(
          {device.path, device.alias, bluetoothDeviceGlyphName(device.kind), static_cast<double>(device.batteryPercent),
           BatteryState::Unknown, 0, false, threshold > 0 && device.batteryPercent <= threshold, true,
           connections && connections->recent(device.path, now, previewSeconds, previewMonitor, output),
           connections
               ? connections->started(device.path, now, BatteryConnections::kGlowSeconds, previewMonitor, output)
               : std::nullopt}
      );
    }
    std::ranges::sort(result, [](const Battery& a, const Battery& b) {
      const int ap = a.low ? 0 : a.compact() ? (a.charging() ? 1 : 2) : 3;
      const int bp = b.low ? 0 : b.compact() ? (b.charging() ? 1 : 2) : 3;
      return ap != bp ? ap < bp : a.id < b.id;
    });
    return result;
  }

  inline float batteryWidth(float width, View view, bool compact, bool unread) {
    if (!compact)
      return width;
    if (view == View::Rest)
      return width + 88 + (unread ? 32 : 0);
    if (view == View::Activity
        || view == View::DownloadActivity
        || view == View::TimerActivity
        || view == View::AwakeActivity)
      return width + 64;
    return width;
  }
} // namespace island
