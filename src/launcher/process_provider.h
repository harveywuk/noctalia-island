#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

// Kill Process in the launcher (Raycast's Kill Process): the user's own processes from /proc with
// their CPU share and memory, searched by name or command line. Return ends one with SIGTERM;
// Force Kill sends SIGKILL. Reached with its prefix only, so process names don't crowd the root
// search.
class ProcessProvider : public LauncherProvider {
public:
  struct Process {
    pid_t pid = 0;
    std::string name;
    std::string command;
    double cpuPercent = 0.0;
    std::uint64_t residentBytes = 0;
  };

  [[nodiscard]] std::string_view defaultPrefix() const override { return "kill"; }
  [[nodiscard]] std::string_view id() const override { return "Processes"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "cpu"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

  // The current user's processes, busiest first. Exposed for tests through `procRoot`.
  [[nodiscard]] static std::vector<Process> scan(std::string_view procRoot = "/proc");
  // Parses the fields of /proc/<pid>/stat this provider needs; exposed for tests.
  struct StatFields {
    std::string comm;
    char state = '?';
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long starttime = 0;
  };
  [[nodiscard]] static std::optional<StatFields> parseStat(std::string_view stat);
};
