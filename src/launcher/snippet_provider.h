#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>

class ClipboardService;
class ConfigService;
class SnippetStore;

// Raycast-style snippets: saved text pasted by name or keyword. Snippets come from config.toml
// (shell.launcher.snippets) and from "Save as Snippet" in clipboard history.
class SnippetProvider : public LauncherProvider {
public:
  SnippetProvider(ConfigService* config, ClipboardService* clipboard, SnippetStore* store);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "snip"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Snippets"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "notes"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }
  [[nodiscard]] bool showsPreview() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherPreview> preview(const LauncherResult& result) const override;

  // Expands {date}, {time}, {datetime} and {clipboard}; exposed for tests.
  [[nodiscard]] static std::string
  expand(std::string_view text, std::chrono::system_clock::time_point now, std::string_view clipboardText);

private:
  struct Entry {
    std::string id;
    std::string name;
    std::string keyword;
    std::string text;
    bool saved = false;
  };

  [[nodiscard]] std::vector<Entry> entries() const;
  [[nodiscard]] std::optional<Entry> entryFor(std::string_view resultId) const;
  [[nodiscard]] std::vector<LauncherResult> search(std::string_view text, bool listAll) const;
  [[nodiscard]] std::string expandedText(const Entry& entry) const;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  SnippetStore* m_store = nullptr;
};
