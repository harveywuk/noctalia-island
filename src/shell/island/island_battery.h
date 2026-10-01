#pragma once

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/island/island_preview_target.h"
#include "shell/island/island_state.h"
#include "system/battery_warning_monitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace island {
  // Key connection age by BlueZ identity, independently of when battery data arrives.
  class BatteryConnections {
  public:
    using Clock = std::chrono::steady_clock;
    void update(
        const std::vector<BluetoothDeviceInfo>& devices, Clock::time_point now, std::string_view focusedOutput = {}
    ) {
      std::erase_if(m_connected, [&](const auto& entry) {
        return std::ranges::none_of(devices, [&](const auto& device) {
          return device.connected && device.path == entry.first;
        });
      });
      for (const auto& device : devices)
        if (device.connected)
          m_connected.try_emplace(device.path, Connection{now, PreviewTarget{std::string(focusedOutput)}});
    }
    void reconcileOutputs(const std::vector<std::string>& available, std::string_view focused) {
      for (auto& [path, connection] : m_connected)
        connection.target.reconcile(available, focused);
    }
    bool recent(
        const std::string& path, Clock::time_point now, int seconds = 5, std::string_view setting = "all",
        std::string_view output = {}
    ) const {
      const auto found = m_connected.find(path);
      return found != m_connected.end()
          && now < found->second.started + std::chrono::seconds(seconds)
          && found->second.target.matches(setting, output);
    }
    std::optional<Clock::time_point> nextExpiry(Clock::time_point now, int seconds = 5) const {
      std::optional<Clock::time_point> next;
      for (const auto& [path, connected] : m_connected) {
        const auto deadline = connected.started + std::chrono::seconds(seconds);
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
  };

  struct Battery {
    std::string id, name, icon;
    double percentage = 0;
    BatteryState state = BatteryState::Unknown;
    std::int64_t seconds = 0;
    bool system = false, low = false;
    bool bluetooth = false, recentlyConnected = false;
    bool charging() const { return state == BatteryState::Charging; }
    bool compact() const { return bluetooth ? low || recentlyConnected : charging() || low || !system; }
  };

  inline std::string batteryAddress(std::string address) {
    std::ranges::transform(address, address.begin(), [](unsigned char c) { return std::tolower(c); });
    return address;
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
           system ? "battery-4" : batteryDeviceGlyphName(device.type), state.percentage, state.state,
           state.state == BatteryState::Charging          ? state.timeToFull
               : state.state == BatteryState::Discharging ? state.timeToEmpty
                                                          : 0,
           system,
           threshold > 0
               && state.percentage <= threshold
               && (wireless || !batteryStatePlugged(state.state).value_or(false)),
           wireless,
           wireless && connections && connections->recent(bt->path, now, previewSeconds, previewMonitor, output)}
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
          {device.path, device.alias, "bluetooth", static_cast<double>(device.batteryPercent), BatteryState::Unknown, 0,
           false, threshold > 0 && device.batteryPercent <= threshold, true,
           connections && connections->recent(device.path, now, previewSeconds, previewMonitor, output)}
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
    if (view == View::Activity || view == View::DownloadActivity || view == View::TimerActivity)
      return width + 64;
    return width;
  }
} // namespace island
