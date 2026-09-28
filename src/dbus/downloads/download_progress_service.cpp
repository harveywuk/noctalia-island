#include "dbus/downloads/download_progress_service.h"
#include "dbus/session_bus.h"
#include "system/desktop_entry.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <glib.h>

DownloadProgressService::DownloadProgressService(SessionBus& bus) : m_bus(bus), m_steam(g_get_home_dir()) {
  m_steamPoll.startRepeating(std::chrono::seconds(2), [this] {
    auto transfers = m_steam.read();
    if (transfers == m_steamTransfers) return;
    m_steamTransfers = std::move(transfers);
    if (changed) changed();
  });
  m_updateSlot = bus.connection().addMatch(
      "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'",
      [this](sdbus::Message message) {
        try {
          std::string uri;
          std::map<std::string, sdbus::Variant> properties;
          message >> uri >> properties;
          constexpr std::string_view prefix = "application://";
          if (!uri.starts_with(prefix)) return;
          const auto id = uri.substr(prefix.size());
          if (id.empty() || id.size() > 256 || id.find('/') != std::string::npos || !id.ends_with(".desktop")) return;
          const auto key = std::pair{std::string(message.getSender()), id};
          if (!m_entries.contains(key) && m_entries.size() >= 128) return;
          auto entry = m_entries.contains(key) ? m_entries.at(key) : Entry{.desktopId = id};
          if (const auto it = properties.find("progress"); it != properties.end()) {
            const double progress = it->second.get<double>();
            if (!std::isfinite(progress)) return;
            entry.progress = std::clamp(progress, 0.0, 1.0);
          }
          if (const auto it = properties.find("progress-visible"); it != properties.end())
            entry.visible = it->second.get<bool>();
          // Ignore badge-only messages; they aren't evidence of a transfer.
          if (!m_entries.contains(key) && !properties.contains("progress") && !properties.contains("progress-visible")) return;
          m_entries[key] = std::move(entry);
          if (changed) changed();
        } catch (const sdbus::Error&) {
          // A malformed update must not affect another application's progress.
        }
      }, sdbus::return_slot);
  m_ownerSlot = bus.connection().addMatch(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
      [this](sdbus::Message message) {
        std::string name, oldOwner, newOwner;
        message >> name >> oldOwner >> newOwner;
        if (!name.starts_with(':') || !newOwner.empty()) return;
        if (std::erase_if(m_entries, [&](const auto& pair) { return pair.first.first == name; }) && changed) changed();
      }, sdbus::return_slot);
  // Announce a compatible listener so libunity publishers resend their state.
  // Another dock may already own this name; signal listening still works then.
  try {
    bus.connection().requestName(sdbus::ServiceName{"com.canonical.Unity"});
    m_ownsName = true;
  } catch (const sdbus::Error&) {}
}

DownloadProgressService::~DownloadProgressService() {
  if (m_ownsName) {
    try { m_bus.connection().releaseName(sdbus::ServiceName{"com.canonical.Unity"}); }
    catch (const sdbus::Error&) {}
  }
}

std::vector<DownloadProgress> DownloadProgressService::active() const {
  std::vector<DownloadProgress> result;
  for (const auto& [key, entry] : m_entries) {
    if (!entry.visible) continue;
    auto name = entry.desktopId.substr(0, entry.desktopId.size() - 8);
    const auto& apps = desktopEntries();
    const auto found = std::ranges::find_if(apps, [&](const auto& app) {
      return app.id == entry.desktopId || app.id + ".desktop" == entry.desktopId;
    });
    if (found != apps.end()) name = found->name;
    result.push_back({entry.desktopId, name, entry.progress});
  }
  if (std::ranges::none_of(result, [](const auto& entry) { return entry.desktopId == "steam.desktop"; }))
    for (const auto& transfer : m_steamTransfers)
      result.push_back({"steam.desktop", transfer.name, 0, false, transfer.phase});
  return result;
}
