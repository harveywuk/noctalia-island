#include "shell/island/island_connection.h"
#include "tests/test_check.h"

#include <limits>

int main() {
  using namespace std::chrono_literals;
  using namespace island;
  const BatteryConnections::Clock::time_point start{};
  BatteryConnections connections;
  BluetoothDeviceInfo headphones;
  headphones.path = "/bluez/headphones";
  headphones.address = "AA:BB:CC:DD:EE:FF";
  headphones.alias = "My headphones";
  headphones.kind = BluetoothDeviceKind::Headphones;
  headphones.connected = true;
  connections.update({headphones}, start, "DP-1");
  auto preview = [&](auto now, const AudioNode* sink = nullptr) {
    return deviceConnection(connections, {headphones}, {}, sink, now, 5, "focused", "DP-1");
  };
  TEST_CHECK(preview(start)->name == "My headphones");
  TEST_CHECK(preview(start)->icon == "bluetooth-device-headphones");
  TEST_CHECK(!preview(start)->percentage && !preview(start)->audioOutput);
  TEST_CHECK(preview(start)->panel == "audio"); // Connected audio devices need controls before becoming the output.
  const auto firstAction = preview(start)->actionKey();
  TEST_CHECK(!connections.preview(start, 0));
  TEST_CHECK(!connections.preview(start, 5, "focused", "DP-2"));
  TEST_CHECK(connections.preview(start, 5, "DP-2", "DP-2"));
  headphones.hasBattery = true;
  headphones.batteryPercent = 75;
  connections.update({headphones}, start + 3s, "DP-2");
  TEST_CHECK(preview(start + 4s)->percentage == 75);
  TEST_CHECK(preview(start + 4s)->actionKey() == firstAction);
  TEST_CHECK(!preview(start + 5s)); // Data updates do not restart the event.
  headphones.batteryPercent = 0;
  TEST_CHECK(preview(start + 4s)->percentage == 0); // Real empty batteries stay distinct from unknown.
  headphones.batteryPercent = 255;
  TEST_CHECK(!preview(start + 4s)->percentage);

  AudioNode sink;
  sink.name = "bluez_output.aa_bb_cc_dd_ee_ff.1";
  sink.mediaClass = "Audio/Sink";
  sink.isDefault = true;
  TEST_CHECK(preview(start + 4s, &sink)->audioOutput);
  sink.name = "bluez_output.AA:BB:CC:DD:EE:FF";
  TEST_CHECK(bluetoothAudioOutput(headphones, &sink));
  sink.isDefault = false;
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink));
  sink.isDefault = true;
  sink.available = false;
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink));
  sink.available = true;
  sink.mediaClass = "Audio/Source";
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink));
  sink.mediaClass = "Audio/Sink";
  sink.name = "bluez_output.AA_BB_CC_DD_EE_FF00.1";
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink));
  sink.name = "renamed-output";
  sink.description = headphones.alias;
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink)); // Labels never establish identity.
  sink.bluetoothAddress = "aa:bb:cc:dd:ee:ff";
  TEST_CHECK(bluetoothAudioOutput(headphones, &sink));
  sink.name = "bluez_output.AA_BB_CC_DD_EE_FF.1";
  sink.bluetoothAddress = "11:22:33:44:55:66";
  TEST_CHECK(!bluetoothAudioOutput(headphones, &sink)); // Explicit metadata overrides a stale name.
  TEST_CHECK(normalizedBluetoothAddress("AA:BB:CC:DD:EE:FF") == "aabbccddeeff");
  TEST_CHECK(normalizedBluetoothAddress("AA-BB-CC-DD-EE-FF").empty());
  TEST_CHECK(normalizedBluetoothAddress("AA:BB:CC:DD:EE:GG").empty());

  auto controller = headphones;
  controller.path = "/bluez/controller";
  controller.address = "11:22:33:44:55:66";
  controller.kind = BluetoothDeviceKind::Gamepad;
  connections.update({headphones, controller}, start + 1s, "DP-2");
  TEST_CHECK(connections.preview(start + 2s, 5)->path == controller.path);
  TEST_CHECK(deviceConnection(connections, {headphones, controller}, {}, nullptr, start + 2s, 5)->panel == "bluetooth");
  connections.update({headphones}, start + 2s);
  TEST_CHECK(!connections.preview(start + 2s, 5)); // Removing the newest never replays the previous card.
  headphones.connected = false;
  connections.update({headphones}, start + 3s);
  headphones.connected = true;
  connections.update({headphones}, start + 4s, "DP-1");
  TEST_CHECK(preview(start + 4s)->actionKey() != firstAction); // A reconnect is a new action, even at the same path.
  connections.reconcileOutputs({"DP-2"}, "DP-2");
  TEST_CHECK(connections.preview(start + 5s, 5, "focused", "DP-2"));
  connections.reconcileOutputs({"DP-1", "DP-2"}, "DP-1");
  TEST_CHECK(!connections.preview(start + 5s, 5, "focused", "DP-1"));
  TEST_CHECK(!connections.preview(start + 9s, 5));

  UPowerDeviceInfo mouse;
  mouse.path = "/upower/mouse";
  mouse.model = "USB mouse";
  mouse.type = UPowerDeviceType::Mouse;
  mouse.isPresent = true;
  mouse.state.percentage = 42;
  mouse.state.state = BatteryState::Discharging;
  connections.updatePower({mouse}, start + 10s, "DP-1", {headphones});
  auto wired = deviceConnection(connections, {headphones}, {mouse}, nullptr, start + 11s, 5);
  TEST_CHECK(wired && wired->name == "USB mouse" && wired->percentage == 42);
  TEST_CHECK(wired->panel.empty()); // A wired battery is not a Bluetooth control target.
  mouse.state.percentage = std::numeric_limits<double>::quiet_NaN();
  TEST_CHECK(!deviceConnection(connections, {}, {mouse}, nullptr, start + 11s, 5)->percentage);
  connections.updatePower({}, start + 12s);
  TEST_CHECK(!connections.preview(start + 12s, 5));
  // A duplicate arriving from UPower must not replay an expired Bluetooth card.
  mouse.serial = headphones.address;
  connections.updatePower({mouse}, start + 13s, "DP-1", {headphones});
  TEST_CHECK(!connections.preview(start + 13s, 5));

  // System charging already has its own OSD, never a second generic connection card.
  mouse.type = UPowerDeviceType::Battery;
  mouse.powerSupply = true;
  mouse.serial.clear();
  mouse.state.state = BatteryState::Charging;
  connections.updatePower({mouse}, start + 14s);
  TEST_CHECK(!connections.preview(start + 14s, 5));

  headphones.connected = false;
  connections.update({headphones}, start + 15s);
  headphones.connected = true;
  connections.update({headphones}, start + 16s);
  TEST_CHECK(connections.preview(start + 16s, 5));
  connections.dismissPreview();
  connections.update({headphones}, start + 17s);
  TEST_CHECK(!connections.preview(start + 17s, 5)); // Opening a card consumes it across monitors.
  TEST_CHECK(connections.started(headphones.path, start + 17s, 10, "all", "") == start + 16s);

  TEST_CHECK(!showsStatusIcons(View::Connection));
  TEST_CHECK(size(View::Connection, 64, 24, false).height == 64);
  const auto choose = [](bool notification, bool osd, bool hovered, bool transfer, bool connection) {
    return view(
        notification, osd, hovered, true, true, false, false, true, true, Activity::None, Activity::Media, transfer,
        connection
    );
  };
  TEST_CHECK(choose(false, false, false, false, true) == View::Connection);
  TEST_CHECK(choose(true, false, false, false, true) == View::Notification);
  TEST_CHECK(choose(false, true, false, false, true) == View::Osd);
  TEST_CHECK(choose(false, false, false, true, true) == View::TransferNotice);
  TEST_CHECK(choose(false, false, true, false, true) == View::Media);
  TEST_CHECK(choose(false, false, false, false, false) == View::Activity);
  return 0;
}
