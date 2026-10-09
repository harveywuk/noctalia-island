#pragma once

#include "pipewire/pipewire_service.h"
#include "shell/island/island_battery.h"

namespace island {
  struct DeviceConnection {
    std::string id, name, icon;
    std::optional<double> percentage;
    bool audioOutput = false;
    std::string panel;
    BatteryConnections::Clock::time_point started;

    std::string actionKey() const {
      return "connection:" + id + ":" + std::to_string(started.time_since_epoch().count()) + ":" + panel;
    }
  };

  inline std::string connectionPanel(BluetoothDeviceKind kind) {
    switch (kind) {
    case BluetoothDeviceKind::Headset:
    case BluetoothDeviceKind::Headphones:
    case BluetoothDeviceKind::Earbuds:
    case BluetoothDeviceKind::Speaker:
    case BluetoothDeviceKind::Microphone:
      return "audio";
    default:
      return "bluetooth";
    }
  }

  inline std::string normalizedBluetoothAddress(std::string_view address) {
    if (address.size() != 17)
      return {};
    std::string result;
    for (std::size_t i = 0; i < address.size(); ++i) {
      const auto c = static_cast<unsigned char>(address[i]);
      if (i % 3 == 2) {
        if (c != ':' && c != '_')
          return {};
      } else {
        if (!std::isxdigit(c))
          return {};
        result += static_cast<char>(std::tolower(c));
      }
    }
    return result;
  }

  inline bool bluetoothAudioOutput(const BluetoothDeviceInfo& device, const AudioNode* sink) {
    if (!device.connected || !sink || !sink->isDefault || !sink->available || sink->mediaClass != "Audio/Sink")
      return false;
    const auto address = normalizedBluetoothAddress(device.address);
    if (address.empty())
      return false;
    // Match machine identity, never the friendly label. Prefer BlueZ metadata even
    // when a WirePlumber rule has renamed the node. A conflicting property is authoritative.
    if (!sink->bluetoothAddress.empty())
      return normalizedBluetoothAddress(sink->bluetoothAddress) == address;
    // WirePlumber's default naming scheme embeds the device address before the profile id.
    const std::string_view name = sink->name;
    constexpr std::string_view prefix = "bluez_output.";
    if (!name.starts_with(prefix))
      return false;
    const auto identity = name.substr(prefix.size());
    return identity.size() >= 17
        && (identity.size() == 17 || identity[17] == '.')
        && normalizedBluetoothAddress(identity.substr(0, 17)) == address;
  }

  inline std::optional<DeviceConnection> deviceConnection(
      const BatteryConnections& connections, const std::vector<BluetoothDeviceInfo>& bluetooth,
      const std::vector<UPowerDeviceInfo>& power, const AudioNode* sink, BatteryConnections::Clock::time_point now,
      int seconds, std::string_view monitor = "all", std::string_view output = {}
  ) {
    const auto event = connections.preview(now, seconds, monitor, output);
    if (!event)
      return std::nullopt;
    if (event->bluetooth) {
      const auto device = std::ranges::find(bluetooth, event->path, &BluetoothDeviceInfo::path);
      if (device == bluetooth.end() || !device->connected)
        return std::nullopt;
      DeviceConnection result{
          device->path,
          device->alias.empty() ? i18n::tr("control-center.bluetooth.unknown-device") : device->alias,
          bluetoothDeviceGlyphName(device->kind),
          std::nullopt,
          bluetoothAudioOutput(*device, sink),
          connectionPanel(device->kind),
          event->started
      };
      if (device->hasBattery && device->batteryPercent <= 100)
        result.percentage = device->batteryPercent;
      return result;
    }
    const auto device = std::ranges::find(power, event->path, &UPowerDeviceInfo::path);
    if (device == power.end() || !device->isPresent)
      return std::nullopt;
    DeviceConnection result{
        device->path,
        device->model.empty() ? i18n::tr("power.battery.tooltip.unknown-device") : device->model,
        batteryDeviceGlyphName(device->type),
        std::nullopt,
        false,
        {},
        event->started
    };
    const auto& state = device->state;
    if (std::isfinite(state.percentage)
        && state.percentage >= 0
        && state.percentage <= 100
        && (state.percentage > 0 || state.state != BatteryState::Unknown))
      result.percentage = state.percentage;
    return result;
  }
} // namespace island
