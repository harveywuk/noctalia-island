#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/toml.h"
#include "shell/island/island_state.h"

#include <cassert>

int main() {
  using island::View;
  using island::view;
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
  Diagnostics diagnostics;
  auto table = toml::parse("height = 900\nclock_size = -1\nscale = 1.1\nenabled = true\nouter_progress_ring = true\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.enabled && cfg.height == 72 && cfg.clockSize == 16 && cfg.outerProgressRing);
  table = toml::parse("clock_offset = -100\nexpanded_clock_offset = 100\ncalendar_labels = 'today'\nmedia_artwork_size = 900\nvolume_bar_height = 0\nvolume_show_percentage = true\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.clockOffset == -12 && cfg.expandedClockOffset == 12);
  assert(cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
  assert(cfg.mediaArtworkSize == 80 && cfg.volumeBarHeight == 5 && cfg.volumeShowPercentage);
  table = toml::parse("calendar_labels = 'invalid'\n");
  readInto(table, cfg, islandSchema(), "island", diagnostics);
  assert(cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
}
