#include "shell/desktop/desktop_battery_model.h"

#include "dbus/bluetooth/bluetooth_glyphs.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace desktop_batteries {
  namespace {
    std::string folded(std::string value) {
      std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

    std::string serialAddress(const std::string& serial) {
      if (serial.size() != 17)
        return {};
      for (std::size_t i = 0; i < serial.size(); ++i)
        if (i % 3 == 2 ? serial[i] != ':' : !std::isxdigit(static_cast<unsigned char>(serial[i])))
          return {};
      return folded(serial);
    }

  } // namespace

  std::vector<Device> collect(std::span<const UPowerDeviceInfo> power, std::span<const BluetoothDeviceInfo> bluetooth) {
    std::vector<Device> result;
    for (const auto& device : power) {
      if (!device.isPresent)
        continue;
      const auto bt = std::ranges::find_if(bluetooth, [&](const auto& candidate) {
        return !device.isLaptopBattery()
            && !device.serial.empty()
            && !candidate.address.empty()
            && folded(device.serial) == folded(candidate.address);
      });
      // BlueZ knows when a paired peripheral is no longer connected, even if
      // its UPower object still contains the last reading.
      if (bt != bluetooth.end() && !bt->connected)
        continue;
      const auto address = device.isLaptopBattery() ? std::string{} : serialAddress(device.serial);
      Device item{
          .id = bt != bluetooth.end() ? "bluetooth:" + folded(bt->address)
              : !address.empty()      ? "bluetooth:" + address
                                      : "upower:" + (device.nativePath.empty() ? device.path : device.nativePath),
          .name = bt != bluetooth.end() && !bt->alias.empty() ? bt->alias : device.model,
          .glyph = bt != bluetooth.end() ? bluetoothDeviceGlyphName(bt->kind)
              : device.isLaptopBattery() ? "device-laptop"
                                         : batteryDeviceGlyphName(device.type),
          .state = device.state.state,
          .health = std::isfinite(device.energyFull) && std::isfinite(device.energyFullDesign) ? device.healthPercent()
                                                                                               : std::nullopt,
          .timeToEmpty = std::max<std::int64_t>(0, device.state.timeToEmpty),
          .timeToFull = std::max<std::int64_t>(0, device.state.timeToFull),
          .system = device.isLaptopBattery(),
      };
      if (item.name.empty())
        item.name = i18n::tr(item.system ? "desktop-widgets.cards.this-device" : "desktop-widgets.cards.battery");
      if (std::isfinite(device.state.percentage))
        item.percentage = static_cast<float>(std::clamp(device.state.percentage, 0.0, 100.0));
      if (std::ranges::find(result, item.id, &Device::id) == result.end())
        result.push_back(std::move(item));
    }
    for (const auto& device : bluetooth) {
      if (!device.connected || !device.hasBattery)
        continue;
      const auto id = "bluetooth:" + (device.address.empty() ? device.path : folded(device.address));
      const auto existing = std::ranges::find(result, id, &Device::id);
      if (existing != result.end()) {
        if (!device.batteryFromUPower)
          existing->percentage = static_cast<float>(std::min<unsigned>(100, device.batteryPercent));
        continue;
      }
      result.push_back(
          {.id = id,
           .name = device.alias.empty() ? i18n::tr("desktop-widgets.cards.battery") : device.alias,
           .glyph = bluetoothDeviceGlyphName(device.kind),
           .percentage = static_cast<float>(std::min<unsigned>(100, device.batteryPercent))}
      );
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) {
      return a.system != b.system ? a.system > b.system : a.id < b.id;
    });
    return result;
  }

  std::vector<Device> collect(const DesktopWidgetRuntimeServices& services) {
    return collect(
        services.upower ? services.upower->batteryDevices() : std::vector<UPowerDeviceInfo>{},
        services.bluetooth ? services.bluetooth->devices() : std::vector<BluetoothDeviceInfo>{}
    );
  }

  std::vector<Device> visibleDevices(std::vector<Device> devices, std::span<const std::string> hidden) {
    std::erase_if(devices, [&](const auto& device) { return std::ranges::find(hidden, device.id) != hidden.end(); });
    return devices;
  }

  std::string percentageText(const Device& device) {
    return device.percentage ? std::format("{:.0f}%", *device.percentage)
                             : i18n::tr("desktop-widgets.batteries.unknown");
  }
  bool charging(const Device& device) { return device.state == BatteryState::Charging; }
  bool low(const Device& device) { return device.percentage && *device.percentage <= 20 && !charging(device); }

  std::string statusText(const Device& device) {
    if (device.state == BatteryState::Unknown)
      return {};
    return batteryStateLabel(device.state);
  }
} // namespace desktop_batteries
