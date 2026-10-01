#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/toml.h"
#include "shell/island/island_activity.h"
#include "shell/island/island_media.h"
#include "shell/island/island_state.h"

#include <cassert>

int main() {
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
  using island::Activity;
  using island::View;
  using island::view;
  using Priority = IslandActivityPriority;
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
  assert(!cfg.outerProgressRing);
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
  table = toml::parse("calendar_labels = 'invalid'\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
}
