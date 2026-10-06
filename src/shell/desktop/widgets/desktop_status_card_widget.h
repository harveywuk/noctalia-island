#pragma once

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"
#include "shell/desktop/desktop_widget_services.h"

#include <array>
#include <string>
#include <vector>

class Label;
class Glyph;
class ProgressBar;
class Box;

// Event-driven cards backed by the shell's existing power and activity services.
class DesktopStatusCardWidget final : public DesktopWidget {
public:
  enum class Kind { Batteries, ScreenTime };
  DesktopStatusCardWidget(Kind kind, DesktopWidgetRuntimeServices services, desktop_cards::Size size);
  void create() override;

private:
  struct Item {
    std::string title;
    std::string detail;
    std::string glyph;
    float progress = 0.0F;
    bool operator==(const Item&) const = default;
  };
  struct Row {
    Label* title = nullptr;
    Label* detail = nullptr;
    Glyph* glyph = nullptr;
    ProgressBar* bar = nullptr;
  };
  bool usesCardLayout() const noexcept override { return true; }
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool refresh();
  Kind m_kind;
  DesktopWidgetRuntimeServices m_services;
  desktop_cards::Size m_size;
  std::string m_value;
  std::string m_detail;
  std::vector<Item> m_items;
  std::array<float, 24> m_history{};
  Label* m_heading = nullptr;
  Label* m_valueLabel = nullptr;
  Label* m_detailLabel = nullptr;
  Glyph* m_symbol = nullptr;
  std::array<Row, 6> m_rows{};
  std::array<Box*, 24> m_bars{};
};
