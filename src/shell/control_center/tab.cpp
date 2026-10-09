#include "shell/control_center/tab.h"

#include "ui/builders.h"

#include <memory>

namespace control_center {

  void applySectionCardStyle(Flex& card, float scale, float fillOpacity) {
    card.setCardStyle(scale, fillOpacity);
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::spaceLg * scale);
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
    parent.addChild(std::move(label));
    return ptr;
  }

  void addBody(Flex& parent, const std::string& text, float scale) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  std::unique_ptr<Flex> makeEmptyState(
      const std::string& glyph, const std::string& title, const std::string& detail, float scale, float fillOpacity
  ) {
    auto card = ui::column({.configure = [scale, fillOpacity](Flex& section) {
      applySectionCardStyle(section, scale, fillOpacity);
      section.setAlign(FlexAlign::Center);
      section.setPadding(Style::spaceLg * scale);
    }});
    card->addChild(
        ui::glyph({
            .glyph = glyph,
            .glyphSize = 32.0F * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7F),
        })
    );
    card->addChild(
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 2,
            .textAlign = TextAlign::Center,
        })
    );
    if (!detail.empty()) {
      card->addChild(
          ui::label({
              .text = detail,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 3,
              .textAlign = TextAlign::Center,
          })
      );
    }
    return card;
  }

  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
  }

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
