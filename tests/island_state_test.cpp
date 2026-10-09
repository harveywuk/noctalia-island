#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/toml.h"
#include "shell/island/island_activity.h"
#include "shell/island/island_media.h"
#include "shell/island/island_state.h"
#include "shell/island/island_transfer.h"

#include <cassert>

int main() {
  assert(!island::showsStatusIcons(island::View::Osd));
  assert(island::showsStatusIcons(island::View::Rest));
  assert(island::showsStatusIcons(island::View::Media));
  assert(island::showsStatusIcons(island::View::Notification));
  using namespace std::chrono_literals;
  island::MediaActivity media;
  const auto start = island::MediaActivity::TimePoint{};
  island::PreviewTarget target{"DP-1"};
  target.reconcile({"DP-1", "DP-2"}, "DP-2");
  assert(target.matches("focused", "DP-1") && !target.matches("focused", "DP-2"));
  assert(target.matches("all", "DP-2") && target.matches("DP-2", "DP-2"));
  target.reconcile({"DP-2"}, "DP-2");
  assert(target.matches("focused", "DP-2") && !target.matches("DP-1", "DP-2"));
  target.reconcile({"DP-1", "DP-2"}, "DP-1");
  assert(target.matches("focused", "DP-2")); // Reconnect does not move a surviving preview.
  target.reconcile({}, "");
  assert(!target.matches("focused", "DP-2"));
  island::MediaActivity routed;
  routed.update("track", "Playing", start, "DP-1");
  routed.update("track", "Playing", start + 1s, "DP-2");
  assert(routed.targets("focused", "DP-1"));
  routed.reconcileOutputs({"DP-2"}, "DP-2");
  assert(routed.targets("focused", "DP-2") && !routed.announcing(start + 5s));
  routed.update("next", "Playing", start + 6s, "DP-1");
  assert(routed.targets("focused", "DP-1"));
  media.update("one", "Playing", start);
  assert(!media.announcing(start, 0));
  assert(!media.announcing(start + 2s, 2) && media.announcing(start + 2s, 8));
  assert(media.nextExpiry(start + 2s, 8) == start + 8s);
  assert(media.announcing(start + 4999ms));
  media.update("one", "Playing", start + 4s); // Position/artwork updates and reloads preserve deadlines.
  assert(!media.announcing(start + 5s) && media.compact(start + 5s));
  assert(!media.nextExpiry(start + 5s));
  media.update("two", "Playing", start + 6s);
  assert(media.announcing(start + 10s));
  media.update("two", "Paused", start + 7s);
  assert(!media.compact(start + 7s, 0));
  assert(!media.compact(start + 9s, 2) && media.compact(start + 9s, 8));
  assert(media.nextExpiry(start + 9s, 5, 8) == start + 15s);
  assert(!media.announcing(start + 7s));
  media.update("two", "Paused", start + 9s);
  assert(media.compact(start + 9999ms) && !media.compact(start + 10s));
  media.update("two", "Playing", start + 11s);
  assert(media.compact(start + 20s) && !media.announcing(start + 11s));
  media.update("two", "Paused", start + 12s);
  media.update("other-player", "Paused", start + 13s);
  assert(!media.compact(start + 13s)); // Never transfer a grace period to a different player/track.
  media.update("three", "Playing", start + 14s);
  media.update("three", "Stopped", start + 15s);
  assert(!media.compact(start + 15s) && !media.nextExpiry(start + 15s));
  media.update("four", "Playing", start + 16s);
  media.update("", "", start + 17s);
  assert(!media.compact(start + 17s) && !media.announcing(start + 17s));
  island::MediaActivity staged;
  staged.update("one", "Paused", start);
  assert(!staged.announcing(start));
  staged.update("one", "Playing", start + 1s);
  assert(staged.announcing(start + 5999ms) && !staged.announcing(start + 6s));
  staged.update("one", "Paused", start + 7s);
  staged.update("two", "Paused", start + 8s);
  assert(!staged.announcing(start + 8s));
  staged.update("two", "Playing", start + 9s);
  assert(staged.announcing(start + 13s));
  staged.update("two", "Paused", start + 10s);
  staged.update("two", "Playing", start + 11s);
  assert(!staged.announcing(start + 11s)); // Resume cannot repeat the announcement.
  staged.update("three", "Stopped", start + 12s);
  staged.update("three", "Playing", start + 13s);
  assert(staged.announcing(start + 13s));
  MprisPlayerInfo browser;
  browser.busName = "browser";
  browser.sourceUrl = "https://example.com/radio";
  browser.trackId = "/track/current";
  browser.title = "First song";
  browser.artists = {"First artist"};
  const auto firstKey = island::mediaAnnouncementKey(browser);
  browser.positionUs = 1000000;
  browser.artUrl = "file:///cover.png";
  assert(island::mediaAnnouncementKey(browser) == firstKey);
  browser.title = "Next song";
  assert(island::mediaAnnouncementKey(browser) != firstKey);
  const auto secondKey = island::mediaAnnouncementKey(browser);
  browser.artists = {"Another artist"};
  assert(island::mediaAnnouncementKey(browser) != secondKey);
  using island::Activity;
  using island::View;
  using island::view;
  using Priority = IslandActivityPriority;
  {
    const auto cameraView = [](bool note, bool osd, bool expanded, Activity selected, bool camera,
                               bool screen = false) {
      return view(
          note, osd, expanded, true, true, false, false, true, true, selected, Activity::Media, false, false, false,
          false, false, screen, false, camera
      );
    };
    assert(cameraView(false, false, false, Activity::None, true) == View::Activity);
    assert(cameraView(false, false, true, Activity::Camera, true) == View::Camera);
    assert(cameraView(false, false, true, Activity::Camera, false) == View::Media);
    assert(cameraView(false, false, true, Activity::Media, true) == View::Media);
    assert(cameraView(false, false, true, Activity::Capture, true, true) == View::Capture);
    assert(cameraView(false, false, true, Activity::Camera, true, true) == View::Camera);
    assert(cameraView(true, true, true, Activity::Camera, true) == View::Notification);
    assert(cameraView(false, true, true, Activity::Camera, true) == View::Osd);
    assert(
        view(
            false, false, true, false, false, false, false, true, true, Activity::None, Activity::None, false, false,
            false, false, false, false, false, true
        )
        == View::Camera
    );
    island::ActivitySelection selected;
    selected.update(true, {false, false, false, false, false, false, true});
    assert(selected.selected == Activity::Camera);
    selected.update(true, {true, false, false, true, false, true, true});
    assert(selected.selected == Activity::Camera && selected.switching);
    selected.update(true, {true, false, false, true, false, true, false});
    assert(selected.selected == Activity::Media);
    assert((island::Activities{true, true, true, true, true, true, true}.count() == 7));
  }
  {
    const auto captureView = [](bool notification, bool osd, bool hover, Activity selected, bool screen,
                                bool recording) {
      return view(
          notification, osd, hover, true, true, false, false, true, true, selected, Activity::Media, false, false,
          false, false, false, screen || recording, recording
      );
    };
    assert(captureView(false, false, false, Activity::None, true, false) == View::Activity);
    assert(captureView(false, false, false, Activity::None, false, true) == View::RecordingActivity);
    assert(captureView(false, false, true, Activity::Capture, true, false) == View::Capture);
    assert(captureView(false, false, true, Activity::Capture, false, true) == View::Capture);
    assert(captureView(false, false, true, Activity::Media, false, true) == View::Media);
    assert(captureView(false, false, true, Activity::Capture, false, false) == View::Media);
    assert(captureView(true, true, true, Activity::Capture, true, true) == View::Notification);
    assert(captureView(false, true, false, Activity::None, true, true) == View::Osd);
    island::ActivitySelection selected;
    selected.selected = Activity::Capture;
    selected.update(true, {true, false, false, false, false, true});
    assert(selected.selected == Activity::Capture && selected.switching);
    selected.update(true, {true, false, false, false, false, false});
    assert(selected.selected == Activity::Media);
    selected.update(true, {false, false, false, false, false, true});
    assert(selected.selected == Activity::Capture);
  }
  {
    island::Activities awakeOnly{false, false, false, false, true};
    island::ActivitySelection selected;
    selected.update(true, awakeOnly);
    assert(selected.selected == Activity::Awake && !selected.switching);
    assert(
        view(
            false, false, false, false, false, false, false, true, true, Activity::None, Activity::None, false, false,
            false, false, true
        )
        == View::AwakeActivity
    );
    assert(
        view(
            false, false, true, false, false, false, false, true, true, Activity::Awake, Activity::None, false, false,
            false, false, true
        )
        == View::Awake
    );
    assert(
        view(
            true, false, true, false, false, false, false, true, true, Activity::Awake, Activity::None, false, false,
            false, false, true
        )
        == View::Notification
    );
    assert(
        view(
            false, true, true, false, false, false, false, true, true, Activity::Awake, Activity::None, false, false,
            false, false, true
        )
        == View::Osd
    );
    selected.update(true, {true, false, false, false, true});
    assert(selected.selected == Activity::Awake && selected.switching);
    selected.update(true, {true, false, false, false, false});
    assert(selected.selected == Activity::Media);
    auto order = island::activityOrder(Priority::TimersDownloadsMedia);
    assert(island::secondaryActivity({true, false, false, false, true}, order, Activity::Media) == Activity::Awake);
    island::CompactActivity cycle;
    cycle.update({true, false, false, false, true}, Priority::TimersDownloadsMedia, true, 5, false, start);
    assert(cycle.selected() == Activity::Media);
    cycle.update({true, false, false, false, true}, Priority::TimersDownloadsMedia, true, 5, false, start + 5s);
    assert(cycle.selected() == Activity::Awake);
    cycle.promote(Activity::Awake);
    cycle.update({true, false, false, false, true}, Priority::TimersDownloadsMedia, false, 5, false, start + 6s);
    assert(cycle.selected() == Activity::Awake);
    cycle.update({true, false, false, false, false}, Priority::TimersDownloadsMedia, false, 5, false, start + 7s);
    assert(cycle.selected() == Activity::Media);
  }
  for (const auto& option : kIslandActivityPriority) {
    const auto order = island::activityOrder(option.value);
    island::CompactActivity cycle;
    cycle.update({true, true, true}, option.value, true, 5, false, start);
    assert(cycle.selected() == order[0]);
    cycle.update({true, true, true}, option.value, true, 5, false, start + 5s);
    assert(cycle.selected() == order[1]);
    cycle.update({true, true, true}, option.value, true, 5, false, start + 10s);
    assert(cycle.selected() == order[2]);
    cycle.update({true, true, true}, option.value, true, 5, false, start + 15s);
    assert(cycle.selected() == order[0]);
  }
  island::CompactActivity cycle, independent;
  independent.update({true, true, true}, Priority::MediaDownloadsTimers, false, 5, false, start);
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, false, start);
  assert(cycle.selected() == Activity::Downloads);
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, false, start + 4s);
  assert(cycle.nextExpiry() == start + 5s); // Progress changes and reloads preserve timing.
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, true, start + 4s);
  assert(!cycle.nextExpiry());
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, true, start + 20s);
  assert(cycle.selected() == Activity::Downloads);
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, false, start + 30s);
  assert(cycle.nextExpiry() == start + 31s);
  cycle.update({true, true, false}, Priority::TimersDownloadsMedia, true, 5, false, start + 31s);
  assert(cycle.selected() == Activity::Media); // Skip the inactive timer.
  cycle.update({false, true, false}, Priority::TimersDownloadsMedia, true, 5, false, start + 32s);
  assert(cycle.selected() == Activity::Downloads && !cycle.nextExpiry());
  cycle.update({false, false, false}, Priority::TimersDownloadsMedia, true, 5, false, start + 33s);
  assert(cycle.selected() == Activity::None && !cycle.nextExpiry());
  cycle.update({true, true, true}, Priority::MediaDownloadsTimers, true, 2, false, start + 34s);
  assert(cycle.selected() == Activity::Media && cycle.nextExpiry() == start + 36s);
  cycle.update({true, true, true}, Priority::TimersDownloadsMedia, false, 2, false, start + 35s);
  assert(cycle.selected() == Activity::Timers && !cycle.nextExpiry());
  assert(independent.selected() == Activity::Media);
  // Split Island: the bubble shows the next running activity, and a click on it swaps the two.
  {
    const auto order = island::activityOrder(Priority::TimersDownloadsMedia);
    assert(island::secondaryActivity({true, true, true}, order, Activity::Timers) == Activity::Downloads);
    assert(island::secondaryActivity({true, false, true}, order, Activity::Timers) == Activity::Media);
    assert(island::secondaryActivity({true, false, true}, order, Activity::Media) == Activity::Timers);
    assert(island::secondaryActivity({false, false, true}, order, Activity::Timers) == Activity::None);
    island::CompactActivity split;
    split.update({true, false, true}, Priority::TimersDownloadsMedia, false, 5, false, start);
    assert(split.selected() == Activity::Timers);
    split.promote(Activity::Media);
    split.update({true, false, true}, Priority::TimersDownloadsMedia, false, 5, false, start + 1s);
    assert(split.selected() == Activity::Media); // The swap survives later refreshes.
    split.update({false, false, true}, Priority::TimersDownloadsMedia, false, 5, false, start + 2s);
    assert(split.selected() == Activity::Timers);
    split.update({true, false, true}, Priority::TimersDownloadsMedia, false, 5, false, start + 3s);
    assert(split.selected() == Activity::Timers); // An ended activity loses its promotion.
  }
  assert(
      view(false, false, false, true, false, true, true, true, true, Activity::None, Activity::Media) == View::Activity
  );
  assert(
      view(false, false, false, true, false, true, true, true, true, Activity::None, Activity::Downloads)
      == View::DownloadActivity
  );
  assert(
      view(true, false, false, true, false, true, true, true, true, Activity::None, Activity::Media)
      == View::Notification
  );
  assert(view(false, true, false, true, false, true, true, true, true, Activity::None, Activity::Media) == View::Osd);
  island::ActivitySelection selection;
  {
    island::ActivitySelection expanded;
    expanded.update(true, {true, true, true}, Activity::Timers);
    assert(expanded.selected == Activity::Timers); // Expand the capsule being viewed.
    expanded.selected = Activity::Media;
    expanded.update(true, {true, true, true}, Activity::Timers);
    assert(expanded.selected == Activity::Media); // Refreshes cannot change an explicit selection.
    expanded.update(true, {false, true, true}, Activity::Timers);
    assert(expanded.selected == Activity::Timers); // Ended activity follows compact priority.
    expanded.update(false, {true, true, true}, Activity::Media);
    expanded.update(true, {true, true, true}, Activity::Media);
    assert(expanded.selected == Activity::Media);
    expanded.update(false, {false, true, true});
    expanded.update(true, {false, true, true}, Activity::Media);
    assert(expanded.selected == Activity::Downloads); // Hidden/unavailable cards still fall back.
  }
  {
    std::vector<DownloadProgress> transfers{
        {.desktopId = "steam.desktop", .name = "First", .key = "steam:42"},
        {.desktopId = "steam.desktop", .name = "Second", .key = "steam:43"},
        {.desktopId = "steam.desktop", .name = "Third", .key = "steam:44"}
    };
    std::string lead = "steam:43";
    island::orderTransfers(transfers, lead);
    assert(transfers[0].name == "Second" && transfers[1].name == "First" && transfers[2].name == "Third");
    island::orderTransfers(transfers, lead);
    assert(transfers[0].name == "Second");
    transfers.erase(transfers.begin());
    island::orderTransfers(transfers, lead);
    assert(lead.empty() && transfers[0].name == "First");
    transfers.push_back({.desktopId = "steam.desktop", .name = "New second", .key = "steam:43"});
    island::orderTransfers(transfers, lead);
    assert(transfers[0].name == "First"); // A new transfer cannot inherit a finished job's choice.
  }
  island::ActivitySelection otherMonitor;
  otherMonitor.update(true, {false, true, true});
  otherMonitor.selected = Activity::Timers;
  selection.update(true, {true, false, false});
  assert(selection.selected == Activity::Media && !selection.switching);
  selection.update(true, {true, true, true});
  assert(selection.selected == Activity::Media && selection.switching);
  selection.selected = Activity::Timers;
  selection.update(true, {true, true, true});
  assert(selection.selected == Activity::Timers);
  assert(view(false, false, true, true, true, true, true, true, true, selection.selected) == View::Timers);
  assert(view(true, false, true, true, true, true, true, true, true, selection.selected) == View::Notification);
  assert(view(false, true, true, true, true, true, true, true, true, selection.selected) == View::Osd);
  selection.update(true, {true, true, false});
  assert(selection.selected == Activity::Downloads);
  selection.update(true, {true, false, false});
  assert(selection.selected == Activity::Media && selection.switching);
  selection.update(false, {true, true, true});
  assert(otherMonitor.selected == Activity::Timers && otherMonitor.switching);
  assert(selection.selected == Activity::None && !selection.switching);
  selection.update(true, {true, true, true});
  assert(selection.selected == Activity::Downloads);
  selection.selected = Activity::Media;
  assert(view(false, false, true, false, true, true, true, true, true, selection.selected) == View::Media);
  selection.update(true, {false, false, false});
  assert(selection.selected == Activity::None);
  selection.update(true, {false, false, false, true});
  assert(selection.selected == Activity::Microphone);
  assert(
      view(
          false, false, true, false, false, false, false, true, true, selection.selected, Activity::None, false, false,
          false, true
      )
      == View::Microphone
  );
  assert(
      view(
          true, false, true, false, false, false, false, true, true, selection.selected, Activity::None, false, false,
          false, true
      )
      == View::Notification
  );
  assert(
      view(
          false, true, true, false, false, false, false, true, true, selection.selected, Activity::None, false, false,
          false, true
      )
      == View::Osd
  );
  selection.update(true, {true, false, false, true});
  assert(selection.selected == Activity::Microphone && selection.switching);
  selection.update(true, {true, false, false, false});
  assert(selection.selected == Activity::Media);
  // A track starts, expands on hover, is paused from its own controls, then loses hover.
  assert(view(false, false, false, false, false) == View::Rest);
  assert(view(false, false, false, true, false) == View::Activity);
  assert(view(false, false, true, true, false) == View::Media);
  assert(view(false, false, true, false, true) == View::Media);
  assert(view(false, false, false, false, false) == View::Rest);
  assert(view(false, false, true, false, false) == View::Calendar);
  // An OSD interrupts music, an alert interrupts the OSD, and both restore the hover view.
  assert(view(false, true, true, true, true) == View::Osd);
  assert(view(true, true, true, true, true) == View::Notification);
  assert(view(false, false, true, true, true) == View::Media);
  assert(view(false, false, false, true, false, true) == View::DownloadActivity);
  assert(view(false, false, true, true, false, true) == View::Downloads);
  assert(view(false, true, true, true, false, true) == View::Osd);
  assert(view(true, true, true, true, false, true) == View::Notification);
  assert(island::size(View::DownloadActivity, 64, 24, false).height == 64);
  assert(view(false, false, false, true, false, true, true) == View::TimerActivity);
  assert(view(false, false, true, true, false, true, true) == View::Downloads);
  assert(view(false, false, true, true, false, false, true) == View::Media);
  assert(view(true, false, false, false, false, false, true) == View::Notification);
  assert(view(false, true, false, false, false, false, true) == View::Osd);
  // Disabling hover sections preserves compact activity and alert priority.
  assert(view(false, false, true, true, true, true, false, false, false) == View::Calendar);
  assert(view(false, false, true, true, true, true, false, true, false) == View::Media);
  assert(view(false, false, true, true, true, true, false, false, true) == View::Downloads);
  assert(view(false, false, false, true, false, false, false, false, false) == View::Activity);
  assert(view(false, false, false, true, false, true, false, false, false) == View::DownloadActivity);
  assert(view(true, true, true, true, true, true, false, false, false) == View::Notification);
  assert(view(false, true, true, true, true, true, false, false, false) == View::Osd);
  // Closing a panel returns directly to the live non-hover state, not always idle.
  const auto playingSize = island::size(view(false, false, false, true, false), 64, 24, false);
  const auto idleSize = island::size(view(false, false, false, false, false), 64, 24, false);
  const auto alertSize = island::size(view(true, false, false, true, false), 64, 24, false);
  assert(playingSize.width == 280 && playingSize.height == 64);
  assert(island::size(View::Activity, 64, 24, false, true, 56, true).width == 420);
  assert(idleSize.width == 160 && idleSize.height == 64);
  assert(alertSize.width == 420 && alertSize.height == 170);
  const auto compactCalendar = island::size(View::Calendar, 64, 24, false, false);
  const auto wideCalendar = island::size(View::Calendar, 64, 24, false, true);
  assert(compactCalendar.width < wideCalendar.width);
  const auto smallArtwork = island::size(View::Media, 64, 24, false, true, 40);
  const auto largeArtwork = island::size(View::Media, 64, 24, false, true, 80);
  assert(largeArtwork.height - smallArtwork.height == 24);
  Config before, after;
  after.island.enabled = true;
  const auto change = computeConfigChangeSet(before, after);
  assert(change.island && change.any() && !change.osd && !change.bars);
  using namespace noctalia::config::schema;
  assert(isKnownConfigPath({"island", "enabled"}));
  assert(isKnownConfigPath({"island", "clock_size"}));
  IslandConfig cfg;
  assert(cfg.outerProgressRing); // progress traces the edge by default
  assert(!cfg.glass);
  assert(cfg.hoverOpenDelayMs == 110 && cfg.hoverCloseDelayMs == 180);
  Diagnostics diagnostics;
  auto table = toml::parse("height = 900\nclock_size = -1\nscale = 1.1\nenabled = true\nouter_progress_ring = true\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.enabled && cfg.height == 72 && cfg.clockSize == 16 && cfg.outerProgressRing);
  table = toml::parse(
      "clock_offset = -100\nexpanded_clock_offset = 100\ncalendar_labels = 'today'\nmedia_artwork_size = "
      "900\nvolume_bar_height = 0\nvolume_show_percentage = true\n"
  );
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.clockOffset == -12 && cfg.expandedClockOffset == 12);
  assert(cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
  table = toml::parse(
      "track_preview_seconds = 99\npaused_media_seconds = -1\nbluetooth_preview_seconds = 0\nreveal_on_track_change = "
      "false\n"
  );
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.trackPreviewSeconds == 30 && cfg.pausedMediaSeconds == 0);
  assert(cfg.bluetoothPreviewSeconds == 0 && !cfg.revealOnTrackChange);
  table = toml::parse("hover_open_delay_ms = -10\nhover_close_delay_ms = 3000\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.hoverOpenDelayMs == 0 && cfg.hoverCloseDelayMs == 2000);
  table = toml::parse(
      "activity_priority = 'media-timers-downloads'\nactivity_cycle_seconds = 99\ncycle_activities = true\n"
  );
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(
      cfg.activityPriority == Priority::MediaTimersDownloads && cfg.cycleActivities && cfg.activityCycleSeconds == 30
  );
  table = toml::parse("activity_priority = 'invalid'\nactivity_cycle_seconds = 0\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.activityPriority == Priority::MediaTimersDownloads && cfg.activityCycleSeconds == 1);
  assert(cfg.mediaArtworkSize == 80 && cfg.volumeBarHeight == 5 && cfg.volumeShowPercentage);
  assert(cfg.splitActivities);
  table = toml::parse("split_activities = false\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(!cfg.splitActivities);
  table = toml::parse("calendar_labels = 'invalid'\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
}
