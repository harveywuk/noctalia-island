#include "dbus/bluetooth/bluetooth_service.h"
#include "shell/desktop/desktop_battery_model.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <print>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using namespace desktop_batteries;
  UPowerDeviceInfo laptop{
      .path = "/battery/BAT0",
      .nativePath = "BAT0",
      .model = "Laptop",
      .energyFull = 45,
      .energyFullDesign = 50,
      .type = UPowerDeviceType::Battery,
      .powerSupply = true,
      .isPresent = true,
      .state = {.percentage = 65, .state = BatteryState::Discharging, .timeToEmpty = 7200}
  };
  UPowerDeviceInfo mouse{
      .path = "/battery/mouse",
      .nativePath = "hid-mouse",
      .model = "Mouse",
      .serial = "AA:BB:CC:DD:EE:FF",
      .type = UPowerDeviceType::Mouse,
      .isPresent = true,
      .state = {.percentage = 40, .state = BatteryState::Charging, .timeToFull = 1800}
  };
  BluetoothDeviceInfo bt{
      .address = "aa:bb:cc:dd:ee:ff",
      .alias = "Desk Mouse",
      .kind = BluetoothDeviceKind::Mouse,
      .connected = true,
      .hasBattery = true,
      .batteryPercent = 42
  };
  std::vector<UPowerDeviceInfo> power{mouse, laptop};
  std::vector<BluetoothDeviceInfo> bluetooth{bt};
  auto devices = collect(power, bluetooth);
  expect(devices.size() == 2 && devices[0].system, "system battery first, UPower and BlueZ deduplicated");
  expect(devices[0].health == 90 && devices[0].timeToEmpty == 7200, "reported health and runtime preserved");
  expect(devices[1].percentage == 42 && devices[1].name == "Desk Mouse", "prefer direct Bluetooth reading and alias");
  expect(charging(devices[1]) && devices[1].timeToFull == 1800, "merged device retains UPower charging estimate");
  expect(!devices[1].health, "missing design capacity does not invent health");
  const auto mouseId = devices[1].id;
  expect(collect(power, {})[1].id == mouseId, "UPower serial retains identity before BlueZ discovery");
  power.push_back(mouse);
  expect(collect(power, bluetooth).size() == 2, "duplicate UPower objects do not duplicate one peripheral");
  power.pop_back();
  const std::vector<std::string> hidden{mouseId};
  expect(visibleDevices(devices, hidden).size() == 1, "hidden device filtered");
  bluetooth[0].connected = false;
  expect(collect(power, bluetooth).size() == 1, "disconnected Bluetooth device excludes stale UPower reading");
  bluetooth[0].connected = true;
  bluetooth[0].batteryFromUPower = true;
  devices = collect(power, bluetooth);
  expect(devices[1].percentage == 40, "UPower fallback is not allowed to replace its source reading");
  expect(devices[1].id == mouseId && visibleDevices(devices, hidden).size() == 1, "selection survives reconnect");
  power.erase(power.begin());
  devices = collect(power, bluetooth);
  expect(devices.size() == 2 && devices[1].id == mouseId, "Bluetooth reading survives without UPower object");
  expect(
      devices[1].state == BatteryState::Unknown && devices[1].timeToFull == 0 && !devices[1].health,
      "Bluetooth-only device has no fabricated charge state, time or health"
  );
  power[0].state.percentage = std::numeric_limits<double>::quiet_NaN();
  devices = collect(power, {});
  expect(!devices[0].percentage && !low(devices[0]), "unknown charge stays unknown, not empty or low");
  power[0].state.percentage = -1;
  devices = collect(power, {});
  expect(devices[0].percentage == 0 && low(devices[0]), "zero charge is valid and low");
  power[0].state.percentage = 101;
  expect(collect(power, {})[0].percentage == 100, "readings are clamped to 100 percent");
  power[0].state.percentage = 20;
  power[0].state.state = BatteryState::Charging;
  expect(!low(collect(power, {})[0]), "charging device does not retain low-battery warning");
  power[0].state.state = BatteryState::Discharging;
  expect(low(collect(power, {})[0]), "20 percent is low when not charging");
  power[0].energyFull = std::numeric_limits<double>::quiet_NaN();
  expect(!collect(power, {})[0].health, "invalid capacity does not report fabricated health");
  power[0].isPresent = false;
  expect(collect(power, {}).empty(), "removed battery is omitted");
  expect(collect({}, {}).empty(), "services may have no devices");
  std::println("PASS: device merging, filtering, selection, health, estimates and charge boundaries");
}
