#pragma once

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_stack_rotation.h"
#include "shell/desktop/desktop_widget.h"
#include "shell/desktop/desktop_widget_services.h"
#include "shell/desktop/desktop_widget_setup.h"

class InputArea;
class Label;
class Button;
class Glyph;

class DesktopStackWidget final : public DesktopWidget {
public:
  DesktopStackWidget(
      std::string id, std::vector<DesktopWidgetState> cards,
      std::unordered_map<std::string, WidgetSettingValue> settings, DesktopWidgetRuntimeServices services
  );
  void create() override;
  [[nodiscard]] bool wantsSecondTicks() const override;
  [[nodiscard]] bool needsFrameTick() const override;
  void onFrameTick(float deltaMs, Renderer& renderer) override;
  void setEditorPreview(bool enabled) noexcept override;
  void setInteractionActive(bool active) override;
  [[nodiscard]] std::size_t page() const { return m_page; }
  [[nodiscard]] std::string activeCardType() const {
    return m_page < m_cards.size() ? m_cards[m_page].type : std::string();
  }
  void showPage(std::size_t page);
  void setPinned(bool pinned);
  [[nodiscard]] bool pinned() const { return m_pinned; }

private:
  bool usesCardLayout() const noexcept override { return true; }
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override;
  void selectPage(std::size_t page);
  desktop_stacks::SmartContext smartContext() const;
  void saveState(const char* key, const std::string& value);
  std::string m_id;
  std::vector<DesktopWidgetState> m_cards;
  std::vector<std::unique_ptr<DesktopWidget>> m_widgets;
  std::vector<Node*> m_roots;
  std::vector<InputArea*> m_pageAreas;
  std::vector<Box*> m_dots;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  DesktopWidgetRuntimeServices m_services;
  desktop_cards::Size m_size = desktop_cards::Size::Medium;
  std::size_t m_page = 0;
  bool m_preview = false;
  bool m_autoRotate = false;
  bool m_smartRotate = false;
  desktop_stacks::SmartRotation m_smartRotation;
  desktop_stacks::SmartPages m_smartPages;
  bool m_interacting = false;
  bool m_pinned = false;
  desktop_stacks::RotationSchedule m_rotation;
  InputArea* m_pinArea = nullptr;
  Glyph* m_pinGlyph = nullptr;
  Label* m_empty = nullptr;
  Button* m_configure = nullptr;
};
