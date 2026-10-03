#include "launcher/snippet_store.h"

#include "util/file_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

SnippetStore::SnippetStore() {
  const std::string dir = FileUtils::stateDir();
  m_path = (dir.empty() ? "." : dir) + "/launcher_snippets.json";
}

SnippetStore::SnippetStore(std::string path) : m_path(std::move(path)) {}

const std::vector<SnippetStore::Snippet>& SnippetStore::snippets() {
  ensureLoaded();
  return m_snippets;
}

std::string SnippetStore::add(std::string name, std::string text, std::string keyword) {
  ensureLoaded();
  std::size_t next = 1;
  for (const auto& snippet : m_snippets) {
    try {
      next = std::max(next, static_cast<std::size_t>(std::stoull(snippet.id)) + 1);
    } catch (...) {
    }
  }
  Snippet snippet{
      .id = std::to_string(next), .name = std::move(name), .keyword = std::move(keyword), .text = std::move(text)
  };
  m_snippets.push_back(snippet);
  save();
  return snippet.id;
}

bool SnippetStore::update(std::string_view id, std::string name, std::string text, std::string keyword) {
  ensureLoaded();
  const auto it = std::ranges::find(m_snippets, id, &Snippet::id);
  if (it == m_snippets.end()) {
    return false;
  }
  it->name = std::move(name);
  it->text = std::move(text);
  it->keyword = std::move(keyword);
  save();
  return true;
}

bool SnippetStore::remove(std::string_view id) {
  ensureLoaded();
  const auto removed = std::erase_if(m_snippets, [id](const Snippet& snippet) { return snippet.id == id; });
  if (removed > 0) {
    save();
  }
  return removed > 0;
}

void SnippetStore::ensureLoaded() {
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
      Snippet snippet;
      snippet.id = item.value("id", "");
      snippet.name = item.value("name", "");
      snippet.keyword = item.value("keyword", "");
      snippet.text = item.value("text", "");
      if (!snippet.id.empty()) {
        m_snippets.push_back(std::move(snippet));
      }
    }
  } catch (const nlohmann::json::exception&) {
    // Ignore a malformed file and start fresh.
  }
}

void SnippetStore::save() const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
  nlohmann::json json = nlohmann::json::array();
  for (const auto& snippet : m_snippets) {
    json.push_back({{"id", snippet.id}, {"name", snippet.name}, {"keyword", snippet.keyword}, {"text", snippet.text}});
  }
  std::ofstream file(m_path, std::ios::trunc);
  file << json.dump(2) << '\n';
  if (m_changed) {
    m_changed();
  }
}
