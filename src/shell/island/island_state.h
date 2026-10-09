#pragma once

#include <algorithm>
#include <array>

namespace island {
  enum class View {
    Rest,
    Activity,
    Calendar,
    Media,
    Osd,
    TransferNotice,
    Connection,
    Network,
    Notification,
    DownloadActivity,
    Downloads,
    TimerActivity,
    Timers,
    Microphone,
    AwakeActivity,
    Awake,
    RecordingActivity,
    Capture,
    Camera,
    CaptureMenu,
    CaptureCountdown
  };
  enum class Activity { None, Media, Downloads, Timers, Microphone, Awake, Capture, Camera };

  // Temporary level/toggle feedback owns the capsule until its timeout expires.
  constexpr bool showsStatusIcons(View view) {
    return view != View::Osd
        && view != View::TransferNotice
        && view != View::Connection
        && view != View::Network
        && view != View::CaptureMenu
        && view != View::CaptureCountdown;
  }

  struct Activities {
    bool media = false, downloads = false, timers = false, microphone = false, awake = false, capture = false,
         camera = false;
    constexpr int count() const {
      return int(media) + int(downloads) + int(timers) + int(microphone) + int(awake) + int(capture) + int(camera);
    }
    constexpr bool contains(Activity activity) const {
      return activity == Activity::Media     ? media
          : activity == Activity::Downloads  ? downloads
          : activity == Activity::Timers     ? timers
          : activity == Activity::Microphone ? microphone
          : activity == Activity::Awake      ? awake
          : activity == Activity::Capture    ? capture
          : activity == Activity::Camera     ? camera
                                             : false;
    }
  };

  struct ActivitySelection {
    Activity selected = Activity::None;
    bool switching = false;
    constexpr void
    update(bool expanded, Activities available, Activity preferred = Activity::None, bool passiveDownloads = false) {
      if (!expanded) {
        selected = Activity::None;
        switching = false;
        return;
      }
      switching |= available.count() > 1;
      if (!available.contains(selected)) {
        // Recent results remain selectable, but cannot take over the idle
        // calendar or another activity when expansion first opens.
        if (passiveDownloads)
          available.downloads = false;
        selected = available.contains(preferred) ? preferred
            : available.downloads                ? Activity::Downloads
            : available.media                    ? Activity::Media
            : available.timers                   ? Activity::Timers
            : available.microphone               ? Activity::Microphone
            : available.awake                    ? Activity::Awake
            : available.capture                  ? Activity::Capture
            : available.camera                   ? Activity::Camera
                                                 : Activity::None;
      }
    }
  };

  // The title cycles only live activities. Callers exclude passive history from this list.
  constexpr Activity nextActivity(Activities available, Activity current) {
    constexpr std::array order{Activity::Media, Activity::Downloads, Activity::Timers, Activity::Microphone,
                               Activity::Awake, Activity::Capture,   Activity::Camera};
    const auto found = std::ranges::find(order, current);
    const auto start = found == order.end() ? order.size() - 1 : static_cast<std::size_t>(found - order.begin());
    for (std::size_t step = 1; step <= order.size(); ++step) {
      const auto candidate = order[(start + step) % order.size()];
      if (available.contains(candidate))
        return candidate;
    }
    return Activity::None;
  }

  constexpr float cardWidth(bool compact) { return compact ? 400.0F : 520.0F; }

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
    case View::AwakeActivity:
    case View::RecordingActivity:
      return {300, height};
    case View::Awake:
      return {360, 184};
    case View::CaptureMenu:
      return {380, 270};
    case View::CaptureCountdown:
      return {320, 106};
    case View::Downloads:
      return {360, 160};
    case View::Timers:
      return {360, 12};
    case View::Microphone:
    case View::Capture:
    case View::Camera:
      return {360, 82};
    case View::Calendar:
      return {
          std::max(abbreviatedCalendar ? 348.0F : 286.0F, clockSize * 1.4F * (seconds ? 5.6F : 3.8F) + 40.0F),
          height + 72
      };
    case View::Media:
      return {324, 190 + std::max(0.0F, artworkSize - 56.0F)};
    case View::Osd:
    case View::TransferNotice:
      return {300, 64};
    case View::Connection:
    case View::Network:
      return {340, 64};
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
      Activity compact = Activity::None, bool transferNotice = false, bool connection = false, bool network = false,
      bool microphone = false, bool awake = false, bool capture = false, bool recording = false, bool camera = false,
      bool recentTransfers = false
  ) {
    if (notification)
      return View::Notification;
    if (osd)
      return View::Osd;
    if (transferNotice)
      return View::TransferNotice;
    if (connection && !hovered)
      return View::Connection;
    if (network && !hovered)
      return View::Network;
    if (!hovered) {
      if (recording)
        return View::RecordingActivity;
      if (compact == Activity::Media && playing)
        return View::Activity;
      if (compact == Activity::Downloads && downloads)
        return View::DownloadActivity;
      if (compact == Activity::Timers && timer)
        return View::TimerActivity;
      if (compact == Activity::Awake && awake)
        return View::AwakeActivity;
    }
    if (hovered) {
      if (selected == Activity::Camera && camera)
        return View::Camera;
      if (selected == Activity::Capture && capture)
        return View::Capture;
      if (selected == Activity::Microphone && microphone)
        return View::Microphone;
      if (selected == Activity::Awake && awake)
        return View::Awake;
      if (selected == Activity::Media && hoverMedia && (playing || heldMedia))
        return View::Media;
      if (selected == Activity::Downloads && hoverDownloads && (downloads || recentTransfers))
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
    if (hovered && microphone)
      return View::Microphone;
    if (hovered && capture)
      return View::Capture;
    if (hovered && camera)
      return View::Camera;
    if (awake && (hovered || !playing))
      return hovered ? View::Awake : View::AwakeActivity;
    if (hovered)
      return View::Calendar;
    return playing ? View::Activity : View::Rest;
  }
} // namespace island
