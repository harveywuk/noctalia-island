#include "dbus/downloads/download_progress_service.h"

#include "dbus/session_bus.h"
#include "system/desktop_entry.h"

#include <algorithm>
#include <cmath>
#include <glib.h>
#include <string_view>

DownloadProgressService::DownloadProgressService(SessionBus& bus)
    : m_bus(bus), m_steam(g_get_home_dir()),
      m_steamLeds(
          g_getenv("NOCTALIA_STEAM_LED_DEVICE") ? g_getenv("NOCTALIA_STEAM_LED_DEVICE") : "/dev/valve-leds-shim"
      ) {
  m_steamPoll.startRepeating(std::chrono::seconds(2), [this] {
    auto transfers = m_steam.read();
    const auto results = m_steam.takeResults();
    if (!results.empty() && reported) {
      // The latest error still wins the brief notice. History keeps log order.
      auto notice = results.size() - 1;
      for (std::size_t i = 0; i < results.size(); ++i)
        if (results[i].failed())
          notice = i;
      const auto source = downloadSource("steam.desktop", desktopEntries());
      for (std::size_t i = 0; i < results.size(); ++i)
        reported({source, results[i].name, results[i].reason, results[i].failed()}, i == notice);
    }
    const bool different = transfers != m_steamTransfers;
    m_steamTransfers = std::move(transfers);
    updateLeds();
    if (different && changed)
      changed();
  });
  m_updateSlot = bus.connection().addMatch(
      "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'",
      [this](sdbus::Message message) {
        try {
          std::string uri;
          std::map<std::string, sdbus::Variant> properties;
          message >> uri >> properties;
          constexpr std::string_view prefix = "application://";
          if (!uri.starts_with(prefix))
            return;
          const auto id = uri.substr(prefix.size());
          if (id.empty() || id.size() > 256 || id.find('/') != std::string::npos || !id.ends_with(".desktop"))
            return;
          const auto key = std::pair{std::string(message.getSender()), id};
          if (!m_entries.contains(key) && m_entries.size() >= 128)
            return;
          auto entry = m_entries.contains(key) ? m_entries.at(key) : Entry{.desktopId = id};
          const auto previous = entry.state;
          if (const auto it = properties.find("progress"); it != properties.end()) {
            const double progress = it->second.get<double>();
            if (!std::isfinite(progress))
              return;
            entry.state.progress = std::clamp(progress, 0.0, 1.0);
          }
          if (const auto it = properties.find("progress-visible"); it != properties.end())
            entry.state.visible = it->second.get<bool>();
          // Ignore badge-only messages; they aren't evidence of a transfer.
          if (!m_entries.contains(key) && !properties.contains("progress") && !properties.contains("progress-visible"))
            return;
          const bool finished = previous.completedBy(entry.state);
          m_entries[key] = std::move(entry);
          if (finished && reported)
            reported({.source = downloadSource(id, desktopEntries())}, true);
          if (changed)
            changed();
        } catch (const sdbus::Error&) {
          // A malformed update must not affect another application's progress.
        }
      },
      sdbus::return_slot
  );
  m_ownerSlot = bus.connection().addMatch(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
      [this](sdbus::Message message) {
        std::string name, oldOwner, newOwner;
        message >> name >> oldOwner >> newOwner;
        if (!name.starts_with(':') || !newOwner.empty())
          return;
        if (std::erase_if(m_entries, [&](const auto& pair) { return pair.first.first == name; }) && changed)
          changed();
      },
      sdbus::return_slot
  );
  // Announce a compatible listener so libunity publishers resend their state.
  // Another dock may already own this name; signal listening still works then.
  try {
    bus.connection().requestName(sdbus::ServiceName{"com.canonical.Unity"});
    m_ownsName = true;
  } catch (const sdbus::Error&) {
  }
}

DownloadProgressService::~DownloadProgressService() {
  if (m_ownsName) {
    try {
      m_bus.connection().releaseName(sdbus::ServiceName{"com.canonical.Unity"});
    } catch (const sdbus::Error&) {
    }
  }
}

void DownloadProgressService::updateLeds() {
  // The LED device has no app identity. Attach it only when one game is
  // downloading; ambiguous groups and other phases retain the normal indicator.
  const bool eligible =
      std::ranges::count_if(m_steamTransfers, [](const auto& transfer) { return transfer.phase != "paused"; }) == 1
      && std::ranges::any_of(m_steamTransfers, [](const auto& transfer) { return transfer.phase == "downloading"; })
      && std::ranges::none_of(m_entries, [](const auto& item) {
           return item.second.desktopId == "steam.desktop" && item.second.state.active();
         });
  auto frame = eligible ? m_steamLeds.read() : std::nullopt;
  // Steam can write hundreds of times a second. Sample at 20 Hz, and retry at
  // the ordinary log polling rate while absent, stale, paused or disconnected.
  if (frame && !m_ledPoll.active())
    m_ledPoll.startRepeating(std::chrono::milliseconds(50), [this] { updateLeds(); });
  else if (!frame)
    m_ledPoll.stop();
  if (frame == m_ledFrame)
    return;
  m_ledFrame = std::move(frame);
  if (changed)
    changed();
}

std::vector<DownloadProgress> DownloadProgressService::active() const {
  std::vector<DownloadProgress> result;
  for (const auto& [key, entry] : m_entries) {
    if (!entry.state.active())
      continue;
    const auto source = downloadSource(entry.desktopId, desktopEntries());
    result.push_back({
        .desktopId = entry.desktopId,
        .name = source.name,
        .progress = entry.state.progress,
        .key = "launcher:" + key.first + ":" + entry.desktopId,
        .source = source,
    });
  }
  if (std::ranges::none_of(result, [](const auto& entry) { return entry.desktopId == "steam.desktop"; }))
    for (const auto& transfer : m_steamTransfers)
      result.push_back({
          .desktopId = "steam.desktop",
          .name = transfer.name,
          .determinate = false,
          .phase = transfer.phase,
          .key = "steam:" + transfer.appId,
          .source = downloadSource("steam.desktop", desktopEntries()),
          .leds = transfer.phase == "downloading" ? m_ledFrame : std::nullopt,
      });
  return result;
}
