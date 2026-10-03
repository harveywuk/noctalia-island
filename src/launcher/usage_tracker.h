#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Tracks how many times each launcher result has been activated.
// Providers that opt in via LauncherProvider::trackUsage() get their results
// score-boosted based on activation history, surfacing frequently used entries.
class UsageTracker {
public:
  UsageTracker();

  void record(std::string_view providerId, std::string_view resultId);
  // Remembers a result across providers for the Suggestions section, without touching its score.
  void recordRecent(std::string_view providerId, std::string_view resultId);
  // Recently activated results across every provider, newest first, as (provider id, result id).
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& recent();
  void clear();
  [[nodiscard]] int getCount(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] int getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] std::size_t getRecentlyUsedCount(std::string_view providerId);

private:
  void ensureLoaded();
  void save() const;

  std::string m_usageCountsPath;
  std::string m_recentlyUsedPath;
  std::string m_recentResultsPath;
  bool m_loaded = false;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_counts;
  std::unordered_map<std::string, std::deque<std::string>> m_recentlyUsed;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_recentlyUsedIndex;
  std::vector<std::pair<std::string, std::string>> m_recent;
};
