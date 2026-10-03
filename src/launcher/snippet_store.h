#pragma once

#include <string>
#include <string_view>
#include <vector>

// Snippets saved from the launcher (Create Snippet, or "Save as Snippet" on a clipboard entry). They live in
// the state directory beside the usage counts; snippets written in config.toml are read separately.
class SnippetStore {
public:
  struct Snippet {
    std::string id;
    std::string name;
    std::string keyword;
    std::string text;
  };

  SnippetStore();
  // For tests: a store backed by `path`.
  explicit SnippetStore(std::string path);

  [[nodiscard]] const std::vector<Snippet>& snippets();
  // Saves `text` under `name` and returns the new snippet's id.
  std::string add(std::string name, std::string text, std::string keyword = {});
  bool update(std::string_view id, std::string name, std::string text, std::string keyword);
  bool remove(std::string_view id);

private:
  void ensureLoaded();
  void save() const;

  std::string m_path;
  bool m_loaded = false;
  std::vector<Snippet> m_snippets;
};
