#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct SteamTransfer {
  std::string appId;
  std::string name;
  std::string phase;
  bool operator==(const SteamTransfer&) const = default;
};

struct SteamResult {
  std::string appId;
  // Unprefixed game title, empty when no manifest supplies one.
  std::string name;
  // Empty on success. Otherwise Steam's error, without a recognised depot wrapper.
  std::string reason;
  bool failed() const { return !reason.empty(); }
};

// Read-only activity tracking. Steam's saved byte counters are not live progress.
class SteamActivity {
public:
  explicit SteamActivity(std::filesystem::path home);
  std::vector<SteamTransfer> read();
  std::vector<SteamResult> takeResults() { return std::exchange(m_results, {}); }
  std::optional<SteamResult> takeCompletion() { return takeResult(false); }
  std::optional<SteamResult> takeFailure() { return takeResult(true); }

private:
  std::filesystem::path m_home;
  std::map<std::string, std::string> m_phases;
  std::unordered_set<std::string> m_observedTransfers;
  std::vector<SteamResult> m_results;
  std::optional<SteamResult> takeResult(bool failed) {
    std::optional<SteamResult> last;
    std::erase_if(m_results, [&](const auto& result) {
      if (result.failed() != failed)
        return false;
      last = result;
      return true;
    });
    return last;
  }
  std::uintmax_t m_offset = 0;
  std::uintmax_t m_inode = 0;
  int m_pid = 0;
  std::string m_pending;
};
