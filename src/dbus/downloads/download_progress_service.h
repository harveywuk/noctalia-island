#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>
#include "core/timer_manager.h"
#include "system/steam_activity.h"

class SessionBus;

struct DownloadProgress {
  std::string desktopId;
  std::string name;
  double progress = 0;
  bool determinate = true;
  std::string phase;
  // Symbol for the row and compact ring; empty means the download arrow. Scripts set their own.
  std::string icon;
};

// Desktop applications publish partial LauncherEntry updates, not individual files.
class DownloadProgressService {
public:
  explicit DownloadProgressService(SessionBus& bus);
  ~DownloadProgressService();
  [[nodiscard]] std::vector<DownloadProgress> active() const;
  std::function<void()> changed;

private:
  struct Entry { std::string desktopId; double progress = 0; bool visible = false; };
  SessionBus& m_bus;
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  sdbus::Slot m_updateSlot;
  sdbus::Slot m_ownerSlot;
  bool m_ownsName = false;
  SteamActivity m_steam;
  std::vector<SteamTransfer> m_steamTransfers;
  Timer m_steamPoll;
};
