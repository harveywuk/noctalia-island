#pragma once

#include "launcher/launcher_provider.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ClipboardService;

// Finds files and folders under the home directory by name, like Spotlight. The tree is
// indexed on a worker thread the first time the launcher searches, and again when the index
// is stale; queries only filter the in-memory index.
class FileProvider : public LauncherProvider {
public:
  struct Entry {
    std::string path;
    std::string nameLower;
    std::size_t nameOffset = 0; // where the base name starts in path
    bool isDir = false;
  };

  // `root` defaults to $HOME; tests pass a fixture directory.
  explicit FileProvider(ClipboardService* clipboard = nullptr, std::filesystem::path root = {});
  ~FileProvider() override;

  FileProvider(const FileProvider&) = delete;
  FileProvider& operator=(const FileProvider&) = delete;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "file"; }
  [[nodiscard]] std::string_view id() const override { return "Files"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "file"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }

  void setResultsChangedCallback(std::function<void()> callback) override;
  [[nodiscard]] bool isLoading() const override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

  [[nodiscard]] bool showsPreview() const override { return true; }
  [[nodiscard]] std::optional<LauncherPreview> preview(const LauncherResult& result) const override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

  // Walks `root` the way the worker does. Exposed for tests.
  [[nodiscard]] static std::vector<Entry> scan(const std::filesystem::path& root);
  // Ranks `entries` against `text`, best first, keeping at most `limit`. Exposed for tests.
  [[nodiscard]] static std::vector<LauncherResult>
  search(const std::vector<Entry>& entries, std::string_view text, std::size_t limit, std::string_view home);

private:
  struct State;

  [[nodiscard]] std::vector<LauncherResult> run(std::string_view text, std::size_t limit) const;
  void ensureIndex() const;

  ClipboardService* m_clipboard = nullptr;
  std::filesystem::path m_root;
  std::shared_ptr<State> m_state;
};
