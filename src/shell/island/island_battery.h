#pragma once

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/island/island_state.h"
#include "system/battery_warning_monitor.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace island {
  struct Battery {
    std::string id, name, icon;
    double percentage = 0;
    BatteryState state = BatteryState::Unknown;
    std::int64_t seconds = 0;
    bool system = false, low = false;
    bool charging() const { return state == BatteryState::Charging; }
    bool compact() const { return charging() || low || !system; }
  };

  inline std::string batteryAddress(std::string address) {
    std::ranges::transform(address, address.begin(), [](unsigned char c) { return std::tolower(c); });
    return address;
  }

  // Read the existing services' snapshots; never start another poller or probe devices.
  inline std::vector<Battery> batterySnapshot(
      const std::vector<UPowerDeviceInfo>& power, const std::vector<BluetoothDeviceInfo>& bluetooth,
      const BatteryConfig& config, const UPowerDeviceInfo* systemBattery = nullptr) {
    std::vector<Battery> result;
    for (const auto& device : power) {
      const auto& state = device.state;
      if (!device.isPresent || !std::isfinite(state.percentage) || state.percentage < 0 || state.percentage > 100
          || (state.percentage == 0 && state.state == BatteryState::Unknown))
        continue;
      const auto bt = std::ranges::find_if(bluetooth, [&](const auto& item) {
        return !device.serial.empty() && batteryAddress(item.address) == batteryAddress(device.serial);
      });
      if (bt != bluetooth.end() && !bt->connected) continue;
      const bool system = device.isLaptopBattery() || device.type == UPowerDeviceType::Ups;
      const int threshold = batteryWarningThresholdForDevice(config, device, systemBattery);
      result.push_back({device.path,
          system ? i18n::tr("power.battery.tooltip.device")
                 : !device.model.empty() ? device.model : i18n::tr("power.battery.tooltip.unknown-device"),
          system ? "battery-4" : batteryDeviceGlyphName(device.type), state.percentage, state.state,
          state.state == BatteryState::Charging ? state.timeToFull
              : state.state == BatteryState::Discharging ? state.timeToEmpty : 0,
          system, threshold > 0 && state.percentage <= threshold && !batteryStatePlugged(state.state).value_or(false)});
    }
    for (const auto& device : bluetooth) {
      if (!device.connected || !device.hasBattery || device.batteryPercent > 100) continue;
      const bool duplicate = std::ranges::any_of(power, [&](const auto& item) {
        return !item.serial.empty() && batteryAddress(item.serial) == batteryAddress(device.address)
            && std::ranges::any_of(result, [&](const Battery& battery) { return battery.id == item.path; });
      });
      if (duplicate) continue;
      result.push_back({device.path, device.alias, "bluetooth", static_cast<double>(device.batteryPercent),
                        BatteryState::Unknown, 0, false, false});
    }
    std::ranges::sort(result, [](const Battery& a, const Battery& b) {
      const int ap = a.low ? 0 : a.charging() ? 1 : !a.system ? 2 : 3;
      const int bp = b.low ? 0 : b.charging() ? 1 : !b.system ? 2 : 3;
      return ap != bp ? ap < bp : a.id < b.id;
    });
    return result;
  }

  inline float batteryWidth(float width, View view, bool compact, bool unread) {
    if (!compact) return width;
    if (view == View::Rest) return width + 88 + (unread ? 32 : 0);
    if (view == View::Activity || view == View::DownloadActivity || view == View::TimerActivity) return width + 64;
    return width;
  }
} // namespace island
