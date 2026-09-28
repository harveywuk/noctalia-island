#pragma once

#include <algorithm>

namespace island {
  enum class View { Rest, Activity, Calendar, Media, Osd, Notification, DownloadActivity, Downloads, TimerActivity };

  struct Size {
    float width;
    float height;
  };

  // Shared by the live capsule and the panel's collapse destination.
  constexpr Size size(View view, float height, float clockSize, bool seconds,
                      bool abbreviatedCalendar = true, float artworkSize = 56.0F) {
    switch (view) {
    case View::Activity:
      return {280, height};
    case View::DownloadActivity:
    case View::TimerActivity:
      return {300, height};
    case View::Downloads:
      return {360, 160};
    case View::Calendar:
      return {std::max(abbreviatedCalendar ? 348.0F : 286.0F, clockSize * 1.4F * (seconds ? 5.6F : 3.8F) + 40.0F), height + 72};
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
  constexpr View view(bool notification, bool osd, bool hovered, bool playing, bool heldMedia, bool downloads = false, bool timer = false) {
    if (notification)
      return View::Notification;
    if (osd)
      return View::Osd;
    if (timer && !hovered)
      return View::TimerActivity;
    if (downloads)
      return hovered ? View::Downloads : View::DownloadActivity;
    if (hovered && (playing || heldMedia))
      return View::Media;
    if (hovered)
      return View::Calendar;
    return playing ? View::Activity : View::Rest;
  }
} // namespace island
