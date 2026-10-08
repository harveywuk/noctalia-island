#pragma once

#include "core/timer_manager.h"
#include "dbus/downloads/download_progress.h"
#include "dbus/downloads/launcher_progress.h"
#include "system/steam_activity.h"

#include <functional>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;

// Desktop applications publish partial LauncherEntry updates, not individual files.
class DownloadProgressService {
public:
  explicit DownloadProgressService(SessionBus& bus);
  ~DownloadProgressService();
  [[nodiscard]] std::vector<DownloadProgress> active() const;
  std::function<void()> changed;
  std::function<void(const DownloadSource&)> completed;
  std::function<void(const DownloadSource&)> failed;

private:
  struct Entry {
    std::string desktopId;
    LauncherProgress state;
  };
  SessionBus& m_bus;
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  sdbus::Slot m_updateSlot;
  sdbus::Slot m_ownerSlot;
  bool m_ownsName = false;
  SteamActivity m_steam;
  std::vector<SteamTransfer> m_steamTransfers;
  Timer m_steamPoll;
};
