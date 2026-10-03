#include "launcher/quicklink_store.h"

#include "util/file_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

  constexpr std::string_view kNewIdPrefix = "custom-";

} // namespace

QuicklinkStore::QuicklinkStore() {
  const std::string dir = FileUtils::stateDir();
  m_path = (dir.empty() ? "." : dir) + "/launcher_quicklinks.json";
}

QuicklinkStore::QuicklinkStore(std::string path) : m_path(std::move(path)) {}

const std::vector<LauncherQuicklinkConfig>& QuicklinkStore::quicklinks() {
  ensureLoaded();
  return m_links;
}

std::string QuicklinkStore::put(LauncherQuicklinkConfig link) {
  ensureLoaded();
  if (link.id.empty()) {
    std::size_t next = 1;
    for (const auto& existing : m_links) {
      if (!existing.id.starts_with(kNewIdPrefix)) {
        continue;
      }
      try {
        next = std::max(next, static_cast<std::size_t>(std::stoull(existing.id.substr(kNewIdPrefix.size()))) + 1);
      } catch (...) {
      }
    }
    link.id = std::string(kNewIdPrefix) + std::to_string(next);
  }
  const std::string id = link.id;
  if (const auto it = std::ranges::find(m_links, id, &LauncherQuicklinkConfig::id); it != m_links.end()) {
    *it = std::move(link);
  } else {
    m_links.push_back(std::move(link));
  }
  save();
  return id;
}

bool QuicklinkStore::remove(std::string_view id) {
  ensureLoaded();
  const auto removed = std::erase_if(m_links, [id](const LauncherQuicklinkConfig& link) { return link.id == id; });
  if (removed > 0) {
    save();
  }
  return removed > 0;
}

void QuicklinkStore::ensureLoaded() {
  if (m_loaded) {
    return;
  }
  m_loaded = true;
  std::ifstream file(m_path);
  if (!file.is_open()) {
    return;
  }
  try {
    const auto json = nlohmann::json::parse(file);
    for (const auto& item : json) {
      LauncherQuicklinkConfig link;
      link.id = item.value("id", "");
      link.name = item.value("name", "");
      link.url = item.value("url", "");
      link.keyword = item.value("keyword", "");
      link.glyph = item.value("glyph", "");
      if (!link.id.empty() && !link.url.empty()) {
        m_links.push_back(std::move(link));
      }
    }
  } catch (const nlohmann::json::exception&) {
    // Ignore a malformed file and start fresh.
  }
}

void QuicklinkStore::save() const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
  nlohmann::json json = nlohmann::json::array();
  for (const auto& link : m_links) {
    json.push_back(
        {{"id", link.id}, {"name", link.name}, {"url", link.url}, {"keyword", link.keyword}, {"glyph", link.glyph}}
    );
  }
  std::ofstream file(m_path, std::ios::trunc);
  file << json.dump(2) << '\n';
}
