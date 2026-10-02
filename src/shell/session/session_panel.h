#pragma once

#include "config/config_types.h"
#include "shell/panel/panel.h"
#include "ui/style.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Box;
class Button;
class Flex;
class Glyph;
class InputArea;
class Label;
class Node;
class Renderer;
class Separator;
class ConfigService;
class SessionActionRunner;

// The session menu, modelled on macOS: a compact menu (Sleep, Restart…, Shut Down…,
// Lock Screen, Log Out…) and, for actions with a countdown, the "Are you sure you want
// to shut down your computer now?" alert that runs the action when the countdown ends.
// Both stages live in one panel; switching resizes it in place (the Island morphs).
class SessionPanel : public Panel {
public:
  explicit SessionPanel(ConfigService* config, SessionActionRunner& actionRunner)
      : m_config(config), m_actionRunner(&actionRunner) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  enum class Stage : std::uint8_t {
    Menu,
    Confirm,
  };

  struct MenuRow {
    std::size_t entryIndex = 0;
    InputArea* area = nullptr;
    Box* highlight = nullptr;
    Label* label = nullptr;
    Label* shortcut = nullptr;
  };

  struct PendingConfirm {
    std::size_t index = 0;
    double remainingMs = 0.0;
    int shownSeconds = -1;
  };

  // Content width matches a macOS alert (260pt) plus the panel's own padding.
  static constexpr float kContentWidth = 248.0F;
  static constexpr float kAlertIconSize = 56.0F;

  void doLayout(Renderer& renderer, float width, float height) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void buildMenu(Node& parent, float scale);
  void buildAlert(Node& parent, float scale);
  void layoutMenu(Renderer& renderer, float width);
  void layoutAlert(Renderer& renderer, float width);

  void activateEntry(std::size_t index);
  void showConfirm(std::size_t index);
  void showMenu(std::optional<std::size_t> highlight);
  void applyStage();
  void requestHostResize();
  void executeEntry(std::size_t index);
  void cancelConfirm();
  void setHighlightedRow(std::optional<std::size_t> row);
  void setAlertFocus(bool confirmFocused);
  void updateAlertText();
  void requestRedraw();

  [[nodiscard]] std::vector<SessionPanelActionConfig> effectiveActions() const;
  [[nodiscard]] std::string actionLabel(const SessionPanelActionConfig& cfg) const;
  [[nodiscard]] std::string menuLabel(const SessionPanelActionConfig& cfg) const;
  [[nodiscard]] bool needsConfirm(const SessionPanelActionConfig& cfg) const;
  [[nodiscard]] std::optional<std::size_t> rowForEntry(std::size_t entryIndex) const;
  [[nodiscard]] std::optional<std::size_t> entryForContext(std::string_view context) const;
  [[nodiscard]] Stage stageForContext(std::string_view context) const;
  [[nodiscard]] float menuHeight() const;
  [[nodiscard]] float alertHeightEstimate() const;

  Node* m_rootNode = nullptr;
  Node* m_menuNode = nullptr;
  Node* m_alertNode = nullptr;
  Glyph* m_alertIcon = nullptr;
  Label* m_alertTitle = nullptr;
  Label* m_alertBody = nullptr;
  Button* m_cancelButton = nullptr;
  Button* m_confirmButton = nullptr;
  std::vector<MenuRow> m_rows;
  std::vector<Separator*> m_separators;
  // Separator i sits directly above the row at m_separatorRows[i].
  std::vector<std::size_t> m_separatorRows;

  std::vector<SessionPanelActionConfig> m_visibleEntries;
  Stage m_stage = Stage::Menu;
  std::optional<std::size_t> m_highlightedRow;
  std::optional<PendingConfirm> m_pendingConfirm;
  // The alert was opened straight from IPC (e.g. `panel-open session shutdown`), so
  // Cancel closes the panel instead of returning to a menu nobody saw.
  bool m_confirmOpenedDirectly = false;
  bool m_confirmFocused = true;
  bool m_actionQueued = false;
  float m_measuredAlertHeight = 0.0F;
  // Stage the host last sized the panel for (preferredHeight() records it), so a stage
  // change can tell when the surface still has the other stage's size.
  mutable Stage m_sizedStage = Stage::Menu;
  bool m_resizeQueued = false;
  // Context that reopens the panel in its current stage; used by attached panels,
  // whose size is fixed until they reopen.
  std::string m_reopenContext;
  ConfigService* m_config = nullptr;
  SessionActionRunner* m_actionRunner = nullptr;
};
