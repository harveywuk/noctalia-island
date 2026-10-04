#include "launcher/process_provider.h"

#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {

  constexpr std::size_t kMaxResults = 60;

  [[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  [[nodiscard]] double uptimeSeconds(std::string_view procRoot) {
    const std::string text = readFile(std::filesystem::path(procRoot) / "uptime");
    double seconds = 0.0;
    const auto end = text.find(' ');
    const std::string_view first = std::string_view(text).substr(0, end);
    (void)std::from_chars(first.data(), first.data() + first.size(), seconds);
    return seconds;
  }

  [[nodiscard]] std::optional<uid_t> statusUid(std::string_view status) {
    const auto pos = status.find("\nUid:");
    if (pos == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view line = status.substr(pos + 5);
    line = StringUtils::trimLeftView(line.substr(0, line.find('\n')));
    unsigned long uid = 0;
    const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), uid);
    if (ec != std::errc{}) {
      return std::nullopt;
    }
    return static_cast<uid_t>(uid);
  }

  [[nodiscard]] std::uint64_t statusRssBytes(std::string_view status) {
    const auto pos = status.find("VmRSS:");
    if (pos == std::string_view::npos) {
      return 0;
    }
    std::string_view line = status.substr(pos + 6);
    line = StringUtils::trimLeftView(line.substr(0, line.find('\n')));
    unsigned long long kib = 0;
    (void)std::from_chars(line.data(), line.data() + line.size(), kib);
    return kib * 1024ULL;
  }

  [[nodiscard]] std::string displayCommand(const std::string& cmdline) {
    std::string out;
    out.reserve(cmdline.size());
    for (const char c : cmdline) {
      out.push_back(c == '\0' ? ' ' : c);
    }
    return StringUtils::trim(out);
  }

  [[nodiscard]] std::string baseName(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
  }

} // namespace

std::string ProcessProvider::displayName() const { return i18n::tr("launcher.providers.processes.title"); }

std::optional<ProcessProvider::StatFields> ProcessProvider::parseStat(std::string_view stat) {
  // "pid (comm) state ppid ..." — comm may contain spaces and parentheses, so split at the last ')'.
  const auto open = stat.find('(');
  const auto close = stat.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
    return std::nullopt;
  }
  StatFields fields;
  fields.comm = std::string(stat.substr(open + 1, close - open - 1));
  std::istringstream rest{std::string(stat.substr(close + 1))};
  std::string token;
  // Fields after comm, 1-based from "state" (field 3 of the file): state=1, ppid=2, …, utime=12,
  // stime=13, …, starttime=20.
  for (int index = 1; rest >> token; ++index) {
    switch (index) {
    case 1:
      fields.state = token.empty() ? '?' : token.front();
      break;
    case 12:
      fields.utime = std::stoull(token);
      break;
    case 13:
      fields.stime = std::stoull(token);
      break;
    case 20:
      fields.starttime = std::stoull(token);
      return fields;
    default:
      break;
    }
  }
  return std::nullopt;
}

std::vector<ProcessProvider::Process> ProcessProvider::scan(std::string_view procRoot) {
  std::vector<Process> processes;
  const double uptime = uptimeSeconds(procRoot);
  const double ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
  const uid_t me = getuid();
  const pid_t self = getpid();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(procRoot, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.empty()
        || !std::ranges::all_of(name, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      continue;
    }
    const pid_t pid = static_cast<pid_t>(std::stol(name));
    if (pid == self) {
      continue;
    }
    const std::string status = readFile(entry.path() / "status");
    const auto uid = statusUid(status);
    if (!uid.has_value() || *uid != me) {
      continue;
    }
    const std::string cmdline = readFile(entry.path() / "cmdline");
    if (cmdline.empty()) {
      continue; // kernel threads and zombies
    }
    const auto stat = parseStat(readFile(entry.path() / "stat"));
    if (!stat.has_value() || stat->state == 'Z') {
      continue;
    }
    Process process;
    process.pid = pid;
    process.command = displayCommand(cmdline);
    const std::string first = process.command.substr(0, process.command.find(' '));
    process.name = baseName(first);
    if (process.name.empty()) {
      process.name = stat->comm;
    }
    process.residentBytes = statusRssBytes(status);
    const double lifetime = uptime - static_cast<double>(stat->starttime) / ticks;
    if (lifetime > 0.5 && ticks > 0.0) {
      process.cpuPercent = static_cast<double>(stat->utime + stat->stime) / ticks / lifetime * 100.0;
    }
    processes.push_back(std::move(process));
  }
  std::ranges::sort(processes, [](const Process& a, const Process& b) {
    if (a.cpuPercent != b.cpuPercent) {
      return a.cpuPercent > b.cpuPercent;
    }
    return a.residentBytes > b.residentBytes;
  });
  return processes;
}

std::vector<LauncherResult> ProcessProvider::query(std::string_view text) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  std::vector<LauncherResult> results;
  for (const Process& process : scan()) {
    double score = 0.0;
    if (!needle.empty()) {
      const double nameScore = FuzzyMatch::score(needle, StringUtils::toLower(process.name)) * 2.0;
      const double commandScore = FuzzyMatch::score(needle, StringUtils::toLower(process.command));
      const double pidScore = std::to_string(process.pid) == needle ? 1000.0 : FuzzyMatch::noMatchScore;
      score = std::max({nameScore, commandScore, pidScore});
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }
    LauncherResult result;
    result.id = std::to_string(process.pid);
    result.title = process.name;
    result.subtitle = i18n::tr(
        "launcher.processes.subtitle", "pid", std::to_string(process.pid), "cpu",
        std::format("{:.1f}", process.cpuPercent), "memory", launcher_util::formatByteSize(process.residentBytes)
    );
    result.glyphName = "cpu";
    result.kind = i18n::tr("launcher.kinds.process");
    result.score = score;
    results.push_back(std::move(result));
    if (results.size() >= kMaxResults) {
      break;
    }
  }
  return results;
}

bool ProcessProvider::activate(const LauncherResult& result) {
  pid_t pid = 0;
  const auto [ptr, ec] = std::from_chars(result.id.data(), result.id.data() + result.id.size(), pid);
  if (ec != std::errc{} || pid <= 0) {
    return false;
  }
  return ::kill(pid, SIGTERM) == 0;
}

std::string ProcessProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.kill-process");
}

std::vector<LauncherAction> ProcessProvider::actions(const LauncherResult& /*result*/) const {
  return {{.id = "force", .label = i18n::tr("launcher.actions.force-kill")}};
}

LauncherActionOutcome ProcessProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (actionId != "force") {
    return LauncherActionOutcome::Failed;
  }
  pid_t pid = 0;
  const auto [ptr, ec] = std::from_chars(result.id.data(), result.id.data() + result.id.size(), pid);
  if (ec != std::errc{} || pid <= 0) {
    return LauncherActionOutcome::Failed;
  }
  return ::kill(pid, SIGKILL) == 0 ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
}
