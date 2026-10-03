#include "launcher/alias_store.h"

#include "util/file_utils.h"
#include "util/string_utils.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::string_view kApplicationsProviderId = "Applications";
}

AliasStore::AliasStore() {
  const std::string dir = FileUtils::stateDir();
  m_path = (dir.empty() ? "." : dir) + "/launcher_aliases.json";
}

AliasStore::AliasStore(std::string path) : m_path(std::move(path)) {}

std::string AliasStore::normalize(std::string_view alias) { return StringUtils::toLower(StringUtils::trim(alias)); }

std::optional<AliasStore::Target> AliasStore::parseSpec(std::string_view spec) {
  const std::string trimmed = StringUtils::trim(spec);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  const auto colon = trimmed.find(':');
  if (colon != std::string::npos && colon > 0 && trimmed.find('/') > colon && colon + 1 < trimmed.size()) {
    return Target{.providerId = trimmed.substr(0, colon), .resultId = trimmed.substr(colon + 1)};
  }
  return Target{.providerId = std::string(kApplicationsProviderId), .resultId = trimmed};
}

void AliasStore::setConfigAliases(const std::unordered_map<std::string, std::string>& aliases) {
  m_config.clear();
  for (const auto& [alias, spec] : aliases) {
    const std::string key = normalize(alias);
    if (auto target = parseSpec(spec); target.has_value() && !key.empty()) {
      m_config[key] = std::move(*target);
    }
  }
}

std::optional<AliasStore::Target> AliasStore::find(std::string_view alias) {
  ensureLoaded();
  const std::string key = normalize(alias);
  if (key.empty()) {
    return std::nullopt;
  }
  if (const auto it = m_config.find(key); it != m_config.end()) {
    return it->second;
  }
  if (const auto it = m_saved.find(key); it != m_saved.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::string AliasStore::aliasFor(std::string_view providerId, std::string_view resultId) {
  ensureLoaded();
  for (const auto* map : {&m_config, &m_saved}) {
    for (const auto& [alias, target] : *map) {
      if (target.providerId == providerId && target.resultId == resultId) {
        return alias;
      }
    }
  }
  return {};
}

bool AliasStore::set(std::string_view alias, Target target) {
  ensureLoaded();
  const std::string key = normalize(alias);
  if (key.empty() || key.find(' ') != std::string::npos) {
    return false;
  }
  std::erase_if(m_saved, [&target](const auto& entry) { return entry.second == target; });
  m_saved[key] = std::move(target);
  save();
  return true;
}

bool AliasStore::removeFor(std::string_view providerId, std::string_view resultId) {
  ensureLoaded();
  const auto removed = std::erase_if(m_saved, [&](const auto& entry) {
    return entry.second.providerId == providerId && entry.second.resultId == resultId;
  });
  if (removed > 0) {
    save();
  }
  return removed > 0;
}

void AliasStore::ensureLoaded() {
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
    for (const auto& [alias, item] : json.items()) {
      Target target{.providerId = item.value("provider", ""), .resultId = item.value("id", "")};
      if (!target.providerId.empty() && !target.resultId.empty()) {
        m_saved[normalize(alias)] = std::move(target);
      }
    }
  } catch (const nlohmann::json::exception&) {
    // Ignore a malformed file and start fresh.
  }
}

void AliasStore::save() const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
  nlohmann::json json = nlohmann::json::object();
  for (const auto& [alias, target] : m_saved) {
    json[alias] = {{"provider", target.providerId}, {"id", target.resultId}};
  }
  std::ofstream file(m_path, std::ios::trunc);
  file << json.dump(2) << '\n';
}
