#pragma once

#include "dbus/upower/upower_service.h"
#include "shell/desktop/desktop_widget_services.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

struct BluetoothDeviceInfo;

namespace desktop_batteries {
  struct Device {
    std::string id;
    std::string name;
    std::string glyph;
    std::optional<float> percentage;
    BatteryState state = BatteryState::Unknown;
    std::optional<double> health;
    std::int64_t timeToEmpty = 0;
    std::int64_t timeToFull = 0;
    bool system = false;
    bool operator==(const Device&) const = default;
  };

  // Shared by the card, its detail panel, and the editor's device checklist.
  [[nodiscard]] std::vector<Device>
  collect(std::span<const UPowerDeviceInfo> power, std::span<const BluetoothDeviceInfo> bluetooth);
  [[nodiscard]] std::vector<Device> collect(const DesktopWidgetRuntimeServices& services);
  [[nodiscard]] std::vector<Device> visibleDevices(std::vector<Device> devices, std::span<const std::string> hidden);
  [[nodiscard]] std::string percentageText(const Device& device);
  [[nodiscard]] std::string statusText(const Device& device);
  [[nodiscard]] bool charging(const Device& device);
  [[nodiscard]] bool low(const Device& device);
} // namespace desktop_batteries
