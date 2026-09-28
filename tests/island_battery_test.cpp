#include "shell/island/island_battery.h"
#include "tests/test_check.h"
#include <limits>

int main() {
  BatteryConfig config;
  UPowerDeviceInfo device;
  device.path = "/battery_BAT0";
  device.isPresent = true;
  device.type = UPowerDeviceType::Battery;
  device.powerSupply = true;
  device.state.percentage = 65;
  device.state.state = BatteryState::Discharging;
  device.state.timeToEmpty = 7200;
  auto snapshot = [&] { return island::batterySnapshot({device}, {}, config); };
  TEST_CHECK(!snapshot().front().compact());
  TEST_CHECK(snapshot().front().seconds == 7200);
  device.state.state = BatteryState::Charging;
  device.state.timeToFull = 1800;
  TEST_CHECK(snapshot().front().compact());
  TEST_CHECK(snapshot().front().charging());
  TEST_CHECK(snapshot().front().seconds == 1800);
  device.state.state = BatteryState::FullyCharged;
  TEST_CHECK(!snapshot().front().compact());
  TEST_CHECK(snapshot().front().seconds == 0);
  device.state.state = BatteryState::Discharging;
  device.state.percentage = 5;
  TEST_CHECK(snapshot().front().low);
  config.warningThreshold = 0;
  TEST_CHECK(!snapshot().front().compact());
  device.isPresent = false;
  TEST_CHECK(snapshot().empty());
  device.isPresent = true;
  device.state.percentage = std::numeric_limits<double>::quiet_NaN();
  TEST_CHECK(snapshot().empty());
  device.state.percentage = 0;
  device.state.state = BatteryState::Unknown;
  TEST_CHECK(snapshot().empty());
  device.state.state = BatteryState::Empty;
  TEST_CHECK(snapshot().size() == 1); // A real empty pack is not missing hardware.

  device.type = UPowerDeviceType::Mouse;
  device.powerSupply = false;
  device.serial = "aa:bb:cc:dd:ee:ff";
  device.state.percentage = 42;
  TEST_CHECK(snapshot().front().compact());
  BluetoothDeviceInfo mouse;
  mouse.path = "/bluez/mouse";
  mouse.address = "AA:BB:CC:DD:EE:FF";
  mouse.connected = true;
  mouse.hasBattery = true;
  mouse.batteryPercent = 42;
  TEST_CHECK(island::batterySnapshot({device}, {mouse}, config).size() == 1);
  mouse.connected = false;
  TEST_CHECK(island::batterySnapshot({device}, {mouse}, config).empty());
  mouse.connected = true;
  TEST_CHECK(island::batterySnapshot({}, {mouse}, config).front().percentage == 42);
  mouse.hasBattery = false;
  TEST_CHECK(island::batterySnapshot({}, {mouse}, config).empty());

  UPowerDeviceInfo laptop = device;
  laptop.path = "/laptop";
  laptop.type = UPowerDeviceType::Battery;
  laptop.powerSupply = true;
  laptop.state.state = BatteryState::Charging;
  TEST_CHECK(island::batterySnapshot({device, laptop}, {}, config).front().id == laptop.path);
  config.deviceThresholds.push_back({device.path, 50});
  TEST_CHECK(island::batterySnapshot({laptop, device}, {}, config).front().id == device.path);
  return 0;
}
