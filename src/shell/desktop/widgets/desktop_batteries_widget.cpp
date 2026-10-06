#include "shell/desktop/widgets/desktop_batteries_widget.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "shell/desktop/desktop_battery_ring.h"
#include "ui/builders.h"

#include <algorithm>

DesktopBatteriesWidget::DesktopBatteriesWidget(
    DesktopWidgetRuntimeServices services, desktop_cards::Size size, std::vector<std::string> hiddenDevices
)
    : m_services(services), m_size(size), m_hidden(std::move(hiddenDevices)) {}

void DesktopBatteriesWidget::create() {
  auto node = ui::inputArea({});
  node->setHitTestVisible(canShowDetails());
  node->setClipChildren(true);
  if (canShowDetails()) {
    node->setOnClick([this](const InputArea::PointerData&) {
      requestDetails({.kind = DesktopWidgetDetailsRequest::Kind::Batteries});
    });
    node->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    node->setTooltip(i18n::tr("desktop-widgets.details.batteries"));
  }
  node->addChild(
      ui::label(
          {.out = &m_heading,
           .text = i18n::tr("desktop-widgets.editor.types.batteries"),
           .fontWeight = FontWeight::Medium,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1}
      )
  );
  node->addChild(ui::label({.out = &m_empty, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 3}));
  for (auto& slot : m_slots) {
    auto ring = std::make_unique<DesktopBatteryRing>();
    slot.ring = ring.get();
    node->addChild(std::move(ring));
    node->addChild(ui::label({.out = &slot.name, .fontWeight = FontWeight::Medium, .maxLines = 1}));
    node->addChild(ui::label({.out = &slot.percentage, .maxLines = 1}));
    node->addChild(
        ui::label({.out = &slot.status, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
    );
  }
  for (const auto& child : node->children())
    child->setHitTestVisible(false);
  setRoot(std::move(node));
  refresh();
}

bool DesktopBatteriesWidget::refresh() {
  auto devices = desktop_batteries::collect(m_services);
  const bool hasDevices = !devices.empty();
  devices = desktop_batteries::visibleDevices(std::move(devices), m_hidden);
  const bool changed = devices != m_devices || hasDevices != m_hasDevices;
  m_hasDevices = hasDevices;
  m_devices = std::move(devices);
  return changed;
}
void DesktopBatteriesWidget::doUpdate(Renderer&) {
  if (refresh() && !isLayingOut())
    requestLayout();
}
void DesktopBatteriesWidget::doLayout(Renderer& renderer) {
  refresh();
  const auto card =
      desktop_cards::resolve(m_size, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  const float s = card.scale;
  const bool small = card.size == desktop_cards::Size::Small;
  const bool large = card.size == desktop_cards::Size::Large;
  const bool single = small && m_devices.size() == 1;
  const bool empty = m_devices.empty();
  auto place = [&](Node* node, float x, float y) {
    node->setPosition(Style::rtl() ? card.width - x - node->width() : x, y);
  };
  auto label = [&](Label* node, const std::string& text, float x, float y, float width, float size,
                   TextAlign align = TextAlign::Start) {
    node->setText(text);
    node->setFontSize(size * s);
    node->setTextAlign(align);
    node->setMinWidth(width);
    node->setMaxWidth(width);
    node->measure(renderer);
    place(node, x, y);
  };
  m_heading->setVisible(large || empty);
  label(m_heading, i18n::tr("desktop-widgets.editor.types.batteries"), 0, 0, card.width, Style::fontSizeCaption);
  m_empty->setVisible(empty);
  label(
      m_empty,
      i18n::tr(!m_hasDevices ? "desktop-widgets.cards.no-batteries" : "desktop-widgets.batteries.none-selected"), 0,
      small       ? 112 * s
          : large ? (card.height - 110 * s) * .5F + 94 * s
                  : card.height - 37 * s,
      card.width, Style::fontSizeCaption, TextAlign::Center
  );
  const float rowHeight = (card.height - 36 * s) / 4;
  for (std::size_t i = 0; i < m_slots.size(); ++i) {
    auto& slot = m_slots[i];
    const auto* device = i < m_devices.size() ? &m_devices[i] : nullptr;
    const bool showRing = single || (empty && (small || large)) ? i == 0 : large ? device != nullptr : true;
    slot.ring->setVisible(showRing);
    slot.name->setVisible(device && (large || single));
    slot.percentage->setVisible(device && (!small || single));
    slot.status->setVisible(device && large);
    if (!showRing)
      continue;
    float diameter, x, y;
    if (large && empty) {
      diameter = 78 * s;
      x = (card.width - diameter) * .5F;
      y = (card.height - 110 * s) * .5F;
    } else if (large) {
      diameter = std::min(60 * s, rowHeight - 16 * s);
      x = 0;
      y = 36 * s + static_cast<float>(i) * rowHeight + (rowHeight - diameter) * .5F;
    } else if (single || (small && empty)) {
      diameter = 78 * s;
      x = single ? 0 : (card.width - diameter) * .5F;
      y = single ? 8 * s : 24 * s;
    } else if (small) {
      diameter = 70 * s;
      x = static_cast<float>(i % 2) * (card.width - diameter);
      y = 8 * s + static_cast<float>(i / 2) * (card.height - diameter - 16 * s);
    } else {
      diameter = std::min(78 * s, card.width / 4 - 16 * s);
      x = (static_cast<float>(i) + .5F) * card.width / 4 - diameter * .5F;
      y = (card.height - diameter - 38 * s) * .5F;
    }
    slot.ring->setDevice(device);
    slot.ring->setDiameter(diameter);
    slot.ring->layout(renderer);
    place(slot.ring, x, y);
    if (!device)
      continue;
    slot.percentage->setColor(
        colorSpecFromRole(desktop_batteries::low(*device) ? ColorRole::Error : ColorRole::OnSurface)
    );
    if (large) {
      label(slot.name, device->name, diameter + 16 * s, y + 8 * s, card.width - diameter - 98 * s, Style::fontSizeBody);
      label(
          slot.status, desktop_batteries::statusText(*device), diameter + 16 * s, y + 32 * s,
          card.width - diameter - 98 * s, Style::fontSizeCaption
      );
      label(
          slot.percentage, desktop_batteries::percentageText(*device), card.width - 76 * s, y + 17 * s, 76 * s, 24,
          TextAlign::End
      );
    } else if (single) {
      label(slot.percentage, desktop_batteries::percentageText(*device), 0, 92 * s, card.width, 46);
      label(slot.name, device->name, 0, card.height - 22 * s, card.width, Style::fontSizeCaption);
    } else if (!small) {
      label(
          slot.percentage, desktop_batteries::percentageText(*device), static_cast<float>(i) * card.width / 4,
          y + diameter + 14 * s, card.width / 4, 24, TextAlign::Center
      );
    }
  }
  root()->setSize(card.width, card.height);
}

void DesktopBatteriesWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  for (auto* label : {m_heading, m_empty})
    if (label)
      label->setFontFamily(family);
  for (auto& slot : m_slots)
    for (auto* label : {slot.name, slot.percentage, slot.status})
      if (label)
        label->setFontFamily(family);
}
