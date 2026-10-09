#include "shell/island/island_network.h"
#include "shell/island/island_state.h"
#include "tests/test_check.h"

int main() {
  using namespace std::chrono_literals;
  using namespace island;
  const NetworkActivity::Clock::time_point start{};
  NetworkState offline;
  offline.kind = NetworkConnectivity::None;
  offline.wirelessEnabled = true;
  auto wifi = offline;
  wifi.kind = NetworkConnectivity::Wireless;
  wifi.connected = true;
  wifi.ssid = "Home";
  NetworkActivity activity;
  activity.update(wifi, start);
  TEST_CHECK(!activity.preview(start, 5) && !activity.nextChange()); // Quiet startup, already connected.
  wifi.signalStrength = 80;
  wifi.ipv4 = "192.0.2.2";
  wifi.vpnActive = wifi.vpnConnected = true;
  wifi.scanning = true;
  wifi.frequencyMhz = 5180;
  activity.update(wifi, start + 1s);
  TEST_CHECK(!activity.nextChange()); // Metadata and access-point roaming do not announce a new link.

  activity.update(offline, start + 2s, "DP-1");
  TEST_CHECK(activity.nextChange() == start + 4500ms);
  activity.update(wifi, start + 4s, "DP-2");
  activity.advance(start + 5s);
  TEST_CHECK(!activity.preview(start + 5s, 5)); // Brief dropout and recovery are both silent.

  activity.update(offline, start + 6s, "DP-1");
  activity.update(offline, start + 7s, "DP-2");
  activity.advance(start + 8499ms);
  TEST_CHECK(!activity.preview(start + 8499ms, 5));
  activity.advance(start + 8500ms);
  auto lost = activity.preview(start + 9s, 5, "focused", "DP-1");
  TEST_CHECK(lost && lost->kind == NetworkNotice::Kind::Lost && lost->link.ssid == "Home");
  TEST_CHECK(!activity.preview(start + 9s, 5, "focused", "DP-2"));
  TEST_CHECK(!activity.preview(start + 9s, 0));
  TEST_CHECK(!activity.preview(start + 13500ms, 5));
  activity.update(wifi, start + 14s, "DP-2");
  activity.advance(start + 14749ms);
  TEST_CHECK(!activity.preview(start + 14749ms, 5));
  activity.advance(start + 14750ms);
  auto restored = activity.preview(start + 15s, 5);
  TEST_CHECK(restored && restored->kind == NetworkNotice::Kind::Restored);
  TEST_CHECK(restored->serial != lost->serial && restored->icon() == "wifi");
  activity.dismiss(lost->serial);
  TEST_CHECK(activity.preview(start + 15s, 5)); // A stale action cannot consume the new event.
  activity.dismiss(restored->serial);
  activity.update(wifi, start + 16s);
  TEST_CHECK(!activity.preview(start + 16s, 5));

  wifi.ssid = "Studio";
  activity.update(wifi, start + 17s, "DP-1");
  wifi.signalStrength = 45;
  activity.update(wifi, start + 17500ms, "DP-2");
  activity.advance(start + 17750ms);
  auto changed = activity.preview(start + 18s, 5);
  TEST_CHECK(changed && changed->kind == NetworkNotice::Kind::Connected && changed->link.ssid == "Studio");
  activity.update(wifi, start + 21s);
  TEST_CHECK(activity.preview(start + 22s, 5)->serial == changed->serial);
  TEST_CHECK(!activity.preview(start + 22750ms, 5)); // Updates never extend the five-second lifetime.

  // A direct handover cancels a pending loss and announces only the confirmed replacement.
  activity.update(offline, start + 23s);
  auto wired = wifi;
  wired.kind = NetworkConnectivity::Wired;
  activity.update(wired, start + 24s, "DP-1");
  activity.advance(start + 24750ms);
  auto cable = activity.preview(start + 25s, 5);
  TEST_CHECK(cable && cable->kind == NetworkNotice::Kind::Connected && cable->icon() == "ethernet");
  TEST_CHECK(cable->link.ssid.empty());
  activity.reconcileOutputs({"DP-2"}, "DP-2");
  TEST_CHECK(activity.preview(start + 25s, 5, "focused", "DP-2"));
  TEST_CHECK(!activity.preview(start + 25s, 5, "DP-1", "DP-2"));

  NetworkActivity initial;
  initial.update(offline, start);
  wifi.ssid.clear();
  initial.update(wifi, start + 1s);
  initial.advance(start + 1750ms);
  const auto serial = initial.preview(start + 2s, 5)->serial;
  wifi.ssid = "Late name";
  initial.update(wifi, start + 3s);
  TEST_CHECK(initial.preview(start + 3s, 5)->serial == serial);
  TEST_CHECK(initial.preview(start + 3s, 5)->detail() == "Late name");
  wifi.ssid.clear();
  initial.update(wifi, start + 4s);
  TEST_CHECK(initial.preview(start + 4s, 5)->detail() == "Late name");
  TEST_CHECK(!initial.preview(start + 6750ms, 5));

  // An unfinished activation cannot claim success. Repeated resolving updates
  // also cannot postpone a sustained loss indefinitely.
  wifi.resolving = true;
  initial.update(wifi, start + 7s);
  initial.update(wifi, start + 9s);
  initial.advance(start + 9500ms);
  TEST_CHECK(initial.preview(start + 10s, 5)->kind == NetworkNotice::Kind::Lost);
  NetworkActivity overlay;
  overlay.update(offline, start);
  wifi.kind = NetworkConnectivity::Unknown;
  wifi.resolving = false;
  overlay.update(wifi, start + 1s);
  overlay.advance(start + 5s);
  TEST_CHECK(!overlay.preview(start + 5s, 5));

  TEST_CHECK(!showsStatusIcons(View::Network));
  TEST_CHECK(size(View::Network, 64, 24, false).height == 64);
  const auto choose = [](bool notification, bool osd, bool hovered, bool transfer, bool device) {
    return view(
        notification, osd, hovered, true, true, false, false, true, true, Activity::None, Activity::Media, transfer,
        device, true
    );
  };
  TEST_CHECK(choose(false, false, false, false, false) == View::Network);
  TEST_CHECK(choose(true, false, false, false, false) == View::Notification);
  TEST_CHECK(choose(false, true, false, false, false) == View::Osd);
  TEST_CHECK(choose(false, false, false, true, false) == View::TransferNotice);
  TEST_CHECK(choose(false, false, false, false, true) == View::Connection);
  TEST_CHECK(choose(false, false, true, false, false) == View::Media);
  return 0;
}
