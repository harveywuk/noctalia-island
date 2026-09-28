#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct SteamTransfer {
  std::string appId;
  std::string name;
  std::string phase;
  bool operator==(const SteamTransfer&) const = default;
};

// Read-only activity tracking. Steam's saved byte counters are not live progress.
class SteamActivity {
public:
  explicit SteamActivity(std::filesystem::path home);
  std::vector<SteamTransfer> read();

private:
  std::filesystem::path m_home;
  std::map<std::string, std::string> m_phases;
  std::uintmax_t m_offset = 0;
  std::uintmax_t m_inode = 0;
  int m_pid = 0;
  std::string m_pending;
};
