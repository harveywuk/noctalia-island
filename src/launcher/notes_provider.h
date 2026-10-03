#pragma once

#include "launcher/launcher_provider.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

// Quick notes in the launcher (the capture half of Raycast's Floating Notes): "note call the
// dentist" appends a line to a Markdown notes file; /note lists the latest ones with Copy and
// Delete, and opens the file in your editor.
class NotesProvider : public LauncherProvider {
public:
  struct Note {
    std::size_t line = 0; // index in the file
    std::string stamp;
    std::string text;
  };

  // `file` overrides the notes file; tests pass a temporary one.
  NotesProvider(ClipboardService* clipboard, ConfigService* config, std::filesystem::path file = {});

  [[nodiscard]] std::string_view defaultPrefix() const override { return "note"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Notes"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "note"; }
  [[nodiscard]] bool showsPreview() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherPreview> preview(const LauncherResult& result) const override;

  // Opens (toggles) the Floating Notes window; wired by the application.
  void setOpenFloatingNotesCallback(std::function<void()> callback) { m_openFloatingNotes = std::move(callback); }

  [[nodiscard]] std::filesystem::path file() const;
  // Appends a note line ("- [2026-10-03 19:40] text"); exposed for tests.
  static bool append(const std::filesystem::path& file, std::string_view text);
  // Note lines in the file, newest first.
  [[nodiscard]] static std::vector<Note> read(const std::filesystem::path& file);
  static bool remove(const std::filesystem::path& file, std::size_t line);

private:
  [[nodiscard]] std::vector<LauncherResult> noteResults(std::string_view filter) const;
  [[nodiscard]] static LauncherResult floatingResult(double score);

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  std::filesystem::path m_file;
  std::function<void()> m_openFloatingNotes;
};
