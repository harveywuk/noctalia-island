#pragma once

#include <algorithm>

namespace island {
  enum class View {
    Rest,
    Activity,
    Calendar,
    Media,
    Osd,
    Notification,
    DownloadActivity,
    Downloads,
    TimerActivity,
    Timers
  };
  enum class Activity { None, Media, Downloads, Timers };

  // Temporary level/toggle feedback owns the capsule until its timeout expires.
  constexpr bool showsStatusIcons(View view) { return view != View::Osd; }

  struct Activities {
    bool media = false, downloads = false, timers = false;
    constexpr int count() const { return int(media) + int(downloads) + int(timers); }
    constexpr bool contains(Activity activity) const {
      return activity == Activity::Media    ? media
          : activity == Activity::Downloads ? downloads
          : activity == Activity::Timers    ? timers
                                            : false;
    }
  };

  struct ActivitySelection {
    Activity selected = Activity::None;
    bool switching = false;
    constexpr void update(bool expanded, Activities available) {
      if (!expanded) {
        selected = Activity::None;
        switching = false;
        return;
      }
      switching |= available.count() > 1;
      if (!available.contains(selected))
        selected = available.downloads ? Activity::Downloads
            : available.media          ? Activity::Media
            : available.timers         ? Activity::Timers
                                       : Activity::None;
    }
  };

  struct Size {
    float width;
    float height;
  };

  // Shared by the live capsule and the panel's collapse destination.
  constexpr Size size(
      View view, float height, float clockSize, bool seconds, bool abbreviatedCalendar = true,
      float artworkSize = 56.0F, bool announcing = false
  ) {
    switch (view) {
    case View::Activity:
      return {announcing ? 420.0F : 280.0F, height};
    case View::DownloadActivity:
    case View::TimerActivity:
      return {300, height};
    case View::Downloads:
      return {360, 160};
    case View::Timers:
      return {360, 12};
    case View::Calendar:
      return {
          std::max(abbreviatedCalendar ? 348.0F : 286.0F, clockSize * 1.4F * (seconds ? 5.6F : 3.8F) + 40.0F),
          height + 72
      };
    case View::Media:
      return {324, 190 + std::max(0.0F, artworkSize - 56.0F)};
    case View::Osd:
      return {300, 64};
    case View::Notification:
      return {420, 170};
    case View::Rest:
      return {std::max(160.0F, height * 2.5F) + (seconds ? clockSize * 1.4F : 0), height};
    }
    return {160, height};
  }

  // Orbit priority: a notification wins over OSD; hover reveals the current activity.
  // Keeping paused media open until hover leaves prevents controls moving under the pointer.
  constexpr View view(
      bool notification, bool osd, bool hovered, bool playing, bool heldMedia, bool downloads = false,
      bool timer = false, bool hoverMedia = true, bool hoverDownloads = true, Activity selected = Activity::None,
      Activity compact = Activity::None
  ) {
    if (notification)
      return View::Notification;
    if (osd)
      return View::Osd;
    if (!hovered) {
      if (compact == Activity::Media && playing)
        return View::Activity;
      if (compact == Activity::Downloads && downloads)
        return View::DownloadActivity;
      if (compact == Activity::Timers && timer)
        return View::TimerActivity;
    }
    if (hovered) {
      if (selected == Activity::Media && hoverMedia && (playing || heldMedia))
        return View::Media;
      if (selected == Activity::Downloads && hoverDownloads && downloads)
        return View::Downloads;
      if (selected == Activity::Timers && timer)
        return View::Timers;
    }
    if (timer && !hovered)
      return View::TimerActivity;
    if (downloads && (!hovered || hoverDownloads))
      return hovered ? View::Downloads : View::DownloadActivity;
    if (hovered && hoverMedia && (playing || heldMedia))
      return View::Media;
    if (hovered)
      return View::Calendar;
    return playing ? View::Activity : View::Rest;
  }
} // namespace island
