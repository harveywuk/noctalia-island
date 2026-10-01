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

  using namespace std::chrono_literals;
  island::BatteryConnections connections;
  const island::BatteryConnections::Clock::time_point start{};
  config = BatteryConfig{};
  mouse.hasBattery = true;
  mouse.batteryPercent = 42;
  connections.update({mouse}, start, "DP-1");
  connections.update({mouse}, start + 1s, "DP-2");
  TEST_CHECK(connections.recent(mouse.path, start + 1s, 5, "focused", "DP-1"));
  TEST_CHECK(!connections.recent(mouse.path, start + 1s, 5, "focused", "DP-2"));
  TEST_CHECK(connections.recent(mouse.path, start + 1s, 5, "DP-2", "DP-2"));
  connections.reconcileOutputs({"DP-2"}, "DP-2");
  TEST_CHECK(connections.recent(mouse.path, start + 2s, 5, "focused", "DP-2"));
  TEST_CHECK(!connections.recent(mouse.path, start + 5s, 5, "focused", "DP-2"));
  TEST_CHECK(!connections.recent(mouse.path, start, 0));
  TEST_CHECK(!connections.recent(mouse.path, start + 3s, 2));
  TEST_CHECK(connections.recent(mouse.path, start + 3s, 8));
  TEST_CHECK(connections.nextExpiry(start + 3s, 8) == start + 8s);
  TEST_CHECK(!island::batterySnapshot({}, {mouse}, config, nullptr, &connections, start, 0).front().compact());
  auto wireless = [&](auto now, bool upower = false) {
    return island::batterySnapshot(
        upower ? std::vector{device} : std::vector<UPowerDeviceInfo>{}, {mouse}, config, nullptr, &connections, now
    );
  };
  TEST_CHECK(wireless(start).front().compact());
  TEST_CHECK(wireless(start + 4999ms).front().compact());
  TEST_CHECK(!wireless(start + 5s).front().compact());
  TEST_CHECK(wireless(start + 5s).size() == 1); // Still available in the hover view.
  TEST_CHECK(connections.nextExpiry(start) == start + 5s);
  TEST_CHECK(!connections.nextExpiry(start + 5s));
  mouse.batteryPercent = 41;
  connections.update({mouse}, start + 4s);
  TEST_CHECK(!wireless(start + 5s).front().compact()); // Percentage updates never extend the preview.
  TEST_CHECK(wireless(start + 4s, true).front().compact());
  TEST_CHECK(!wireless(start + 5s, true).front().compact()); // UPower deduplication uses the same BlueZ deadline.
  mouse.batteryPercent = 5;
  TEST_CHECK(wireless(start + 10s).front().low);
  TEST_CHECK(wireless(start + 10s).front().compact());
  config.deviceThresholds.push_back({mouse.address, 0});
  TEST_CHECK(!wireless(start + 10s).front().compact());
  config.deviceThresholds.clear();
  mouse.batteryPercent = 42;
  mouse.connected = false;
  connections.update({mouse}, start + 11s);
  TEST_CHECK(wireless(start + 11s).empty());
  mouse.connected = true;
  mouse.hasBattery = false;
  connections.update({mouse}, start + 12s);
  TEST_CHECK(wireless(start + 12s).empty());
  mouse.hasBattery = true;
  connections.update({mouse}, start + 14s);
  TEST_CHECK(wireless(start + 14s).front().compact());
  TEST_CHECK(!wireless(start + 17s).front().compact()); // Late battery data does not start another five seconds.
  auto second = mouse;
  second.path = "/bluez/second";
  second.address = "00:11:22:33:44:55";
  connections.update({mouse, second}, start + 20s);
  auto both = island::batterySnapshot({}, {mouse, second}, config, nullptr, &connections, start + 21s);
  TEST_CHECK(both.front().id == second.path && both.front().compact());
  mouse.batteryPercent = 3;
  both = island::batterySnapshot({}, {mouse, second}, config, nullptr, &connections, start + 21s);
  TEST_CHECK(both.front().id == mouse.path && both.front().low); // Low charge takes priority.
  connections.update({}, start + 22s);
  TEST_CHECK(!connections.nextExpiry(start + 22s));
  return 0;
}
