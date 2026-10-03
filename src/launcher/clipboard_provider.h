#pragma once

#include "launcher/launcher_provider.h"

class ClipboardService;
class ConfigService;
class SnippetStore;

// Clipboard history in the launcher (Raycast's Clipboard History): search what was copied, paste it
// back, pin it, or save it as a snippet. Reached with its prefix; it stays out of the global search.
class ClipboardProvider : public LauncherProvider {
public:
  ClipboardProvider(ClipboardService* clipboard, ConfigService* config, SnippetStore* snippets);

  void setFormRequestedCallback(std::function<void(LauncherForm)> callback) override {
    m_requestForm = std::move(callback);
  }

  [[nodiscard]] std::string_view defaultPrefix() const override { return "clip"; }
  [[nodiscard]] std::string_view id() const override { return "Clipboard"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "clipboard-text"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool showsPreview() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherPreview> preview(const LauncherResult& result) const override;

  // Whether the last entry pasted from the list was an image, which pastes with a different chord.
  [[nodiscard]] bool lastCopyWasImage() const noexcept { return m_lastCopyWasImage; }
  // The configured image action with the exported image's path filled in; exposed for tests.
  [[nodiscard]] static std::string imageActionCommand(std::string command, std::string_view imagePath);

private:
  [[nodiscard]] std::optional<std::size_t> indexFor(std::string_view storageId) const;
  [[nodiscard]] std::string fullText(std::size_t index) const;
  bool copy(std::string_view storageId, bool promote);
  [[nodiscard]] bool pastes() const;
  [[nodiscard]] std::string imageAction() const;

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  SnippetStore* m_snippets = nullptr;
  std::function<void(LauncherForm)> m_requestForm;
  bool m_lastCopyWasImage = false;
};
