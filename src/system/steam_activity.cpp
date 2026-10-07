#include "system/steam_activity.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string readFile(const std::filesystem::path& path, std::size_t limit = 256 * 1024) {
    std::ifstream file(path, std::ios::binary);
    std::string text(limit, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
  }
  std::string unescape(std::string text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\\' || text[i + 1] == '"'))
        ++i;
      result += text[i];
    }
    return result;
  }
} // namespace

SteamActivity::SteamActivity(std::filesystem::path home) : m_home(std::move(home)) {}

std::vector<SteamTransfer> SteamActivity::read() {
  const auto pidFile = m_home / ".steam/steam.pid";
  int pid = 0;
  std::ifstream(pidFile) >> pid;
  struct stat pidStat{}, processStat{}, logStat{};
  const auto proc = std::filesystem::path("/proc") / std::to_string(pid);
  const bool running = pid > 1
      && stat(pidFile.c_str(), &pidStat) == 0
      && stat(proc.c_str(), &processStat) == 0
      && processStat.st_uid == getuid()
      && readFile(proc / "comm", 64) == "steam\n";
  if (!running) {
    m_phases.clear();
    m_pid = 0;
    m_offset = 0;
    m_inode = 0;
    m_pending.clear();
    m_observedTransfers.clear();
    m_completed = false;
    return {};
  }
  auto root = m_home / ".steam/steam";
  auto log = root / "logs/content_log.txt";
  if (stat(log.c_str(), &logStat) != 0)
    return {};
  const auto length = static_cast<std::uintmax_t>(logStat.st_size);
  const bool replay = pid != m_pid || m_inode != logStat.st_ino || length < m_offset;
  if (replay) {
    m_phases.clear();
    m_pending.clear();
    m_observedTransfers.clear();
    m_completed = false;
    m_pid = pid;
    m_inode = logStat.st_ino;
    // A bounded tail also supports attaching while Steam is downloading.
    m_offset = length > 1024 * 1024 ? length - 1024 * 1024 : 0;
  }
  std::ifstream file(log, std::ios::binary);
  file.seekg(static_cast<std::streamoff>(m_offset));
  std::string chunk(static_cast<std::size_t>(std::min<std::uintmax_t>(length - m_offset, 1024 * 1024)), '\0');
  file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  chunk.resize(static_cast<std::size_t>(file.gcount()));
  m_offset += chunk.size();
  m_pending += chunk;
  std::size_t consumed = 0;
  while (true) {
    const auto end = m_pending.find('\n', consumed);
    if (end == std::string::npos)
      break;
    const auto line = m_pending.substr(consumed, end - consumed);
    consumed = end + 1;
    if (line.size() < 23 || line[0] != '[')
      continue;
    const auto app = line.find("AppID ");
    if (app == std::string::npos)
      continue;
    std::tm tm{};
    tm.tm_isdst = -1;
    std::istringstream timestamp(line.substr(1, 19));
    timestamp >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (timestamp.fail() || std::mktime(&tm) < pidStat.st_mtime - 2)
      continue;
    const auto start = app + 6;
    const auto last = line.find_first_not_of("0123456789", start);
    const auto id = line.substr(start, last - start);
    if (id.empty() || id.size() > 10)
      continue;
    if (line.find("update canceled") != std::string::npos || line.find("update finished") != std::string::npos) {
      if (m_observedTransfers.erase(id) && !replay && line.find("update finished : No Error") != std::string::npos)
        m_completed = true;
      m_phases.erase(id);
      continue;
    }
    const auto changed = line.find("App update changed : ");
    if (changed == std::string::npos)
      continue;
    const auto state = line.substr(changed + 21);
    if (state.find("Stopping") != std::string::npos || state.starts_with("None"))
      m_phases.erase(id);
    else if (m_phases.contains(id) || m_phases.size() < 32) {
      if (state.find("Downloading") != std::string::npos)
        m_phases[id] = "downloading";
      else if (state.find("Staging") != std::string::npos || state.find("Committing") != std::string::npos)
        m_phases[id] = "installing";
      else if (state.find("Verifying") != std::string::npos || state.find("Validating") != std::string::npos)
        m_phases[id] = "verifying";
      else if (state.find("Running Update") != std::string::npos)
        m_phases[id] = "preparing";
      else
        m_phases.erase(id);
    }
    if (m_phases.contains(id) && m_observedTransfers.size() < 32)
      m_observedTransfers.insert(id);
  }
  m_pending.erase(0, consumed);
  if (m_pending.size() > 16384)
    m_pending.clear();
  if (m_phases.empty())
    return {};
  std::vector<std::filesystem::path> libraries{root};
  const auto folders = readFile(root / "steamapps/libraryfolders.vdf");
  static const std::regex pathPattern(R"re("path"\s*"((?:\\.|[^"\\])*)")re");
  for (auto it = std::sregex_iterator(folders.begin(), folders.end(), pathPattern); it != std::sregex_iterator(); ++it)
    libraries.emplace_back(unescape((*it)[1].str()));
  std::vector<SteamTransfer> result;
  static const std::regex namePattern(R"re("name"\s*"((?:\\.|[^"\\])*)")re");
  for (const auto& [id, phase] : m_phases) {
    auto name = std::string("Steam");
    for (const auto& library : libraries) {
      const auto manifest = readFile(library / "steamapps" / ("appmanifest_" + id + ".acf"));
      std::smatch match;
      if (std::regex_search(manifest, match, namePattern)) {
        name += " · " + unescape(match[1].str());
        break;
      }
    }
    result.push_back({id, name, phase});
  }
  return result;
}
