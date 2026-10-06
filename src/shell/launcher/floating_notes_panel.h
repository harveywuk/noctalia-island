#pragma once

#include "core/timer_manager.h"
#include "shell/panel/panel.h"

#include <filesystem>
#include <string>

class ConfigService;
class Flex;
class Input;
class Label;

// Raycast's Floating Notes: a small note window that stays over the apps (a persistent panel,
// so opening the launcher or another panel leaves it up). It edits the quick-notes Markdown
// file in place and saves as you type, so "note …" captures from the launcher appear in it and
// what you write here lists under /note.
class FloatingNotesPanel : public Panel {
public:
  explicit FloatingNotesPanel(ConfigService* config);
  ~FloatingNotesPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(kWidth); }
  [[nodiscard]] float preferredHeight() const override { return scaled(kHeight); }
  [[nodiscard]] bool isPersistent() const noexcept override { return true; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] std::string panelScreenPosition() const override;

  // The notes file the window edits (shell.launcher.notes_file, or ~/Documents/Notes.md).
  [[nodiscard]] std::filesystem::path file() const;

private:
  static constexpr float kWidth = 340.0F;
  static constexpr float kHeight = 300.0F;

  void doLayout(Renderer& renderer, float width, float height) override;
  void load();
  void save();
  void scheduleSave();
  void setStatus(const std::string& text);

  ConfigService* m_config = nullptr;
  Flex* m_container = nullptr;
  Input* m_editor = nullptr;
  Label* m_status = nullptr;
  Label* m_title = nullptr;
  std::filesystem::path m_contextFile;
  std::string m_loaded;
  bool m_dirty = false;
  Timer m_saveTimer;
};
