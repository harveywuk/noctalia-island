#include "shell/notification/notification_center_panel.h"

#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>

namespace {

  constexpr float kColumnWidth = 380.0F;
  constexpr float kFallbackHeight = 640.0F;

} // namespace

NotificationCenterPanel::NotificationCenterPanel(NotificationManager* notifications, CompositorPlatform* platform)
    : m_history(notifications, platform) {}

float NotificationCenterPanel::preferredWidth() const { return scaled(kColumnWidth); }

// Only used if the compositor never assigns the full-height size.
float NotificationCenterPanel::preferredHeight() const { return scaled(kFallbackHeight); }

float NotificationCenterPanel::islandWidth(float availableWidth) const {
  return std::min(scaled(PanelManager::instance().islandCompactLayout() ? 444.0F : 528.0F), availableWidth);
}

float NotificationCenterPanel::islandHeight(float availableHeight) const {
  const float chrome = scaled(2 * (Style::panelPadding + 2.0F))
      + (m_header ? m_header->height() + m_root->gap() : scaled(Style::controlHeightSm));
  return std::min(chrome + m_history.fittedHeight(), std::min(scaled(600.0F), availableHeight * 0.85F));
}

InputArea* NotificationCenterPanel::initialFocusArea() const { return m_close ? m_close->inputArea() : nullptr; }

void NotificationCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  m_history.scrollFocusedInputIntoView(area);
}

void NotificationCenterPanel::create() {
  const float scale = contentScale();
  m_history.setContentScale(scale);
  m_history.setPanelCardOpacity(panelCardOpacity());

  const bool hosted = PanelManager::instance().isIslandOpen();
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceLg * scale,
      .padding = (hosted ? 2.0F : Style::spaceSm) * scale,
  });

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  if (!hosted) {
    // Standalone cards have no sheet, so the toolbar needs its own readable surface.
    header->setCardStyle(scale, panelCardOpacity());
    header->setPadding(Style::spaceSm * scale);
  }
  header->addChild(
      ui::label({
          .text = i18n::tr("control-center.tabs.notifications"),
          .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::SemiBold,
          .maxLines = 1,
          .flexGrow = 1.0F,
      })
  );
  header->addChild(m_history.createHeaderActions());
  header->addChild(
      ui::button({
          .out = &m_close,
          .glyph = "x",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("notifications.close-history"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = 0,
          .radius = Style::controlHeightSm * scale * 0.5F,
          .onClick = [] { PanelManager::instance().close(); },
      })
  );
  m_close->inputArea()->setTabFocusKey("notification-history-close");
  root->addChild(std::move(header));

  auto body = m_history.create();
  m_body = body.get();
  m_body->setFlexGrow(1.0F);
  root->addChild(std::move(body));

  setRoot(std::move(root));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }
  if (onOpened) {
    onOpened();
  }
}

void NotificationCenterPanel::onClose() {
  m_history.onClose();
  m_root = nullptr;
  m_header = nullptr;
  m_body = nullptr;
  m_close = nullptr;
}

void NotificationCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr || m_header == nullptr || m_body == nullptr) {
    return;
  }
  m_root->setSize(width, height);
  m_root->layout(renderer);
  const float pad = (PanelManager::instance().isIslandOpen() ? 2.0F : Style::spaceSm) * contentScale();
  const float bodyHeight = std::max(0.0F, height - pad * 2.0F - m_header->height() - m_root->gap());
  m_history.layout(renderer, std::max(0.0F, width - pad * 2.0F), bodyHeight);
  m_root->layout(renderer);
}

void NotificationCenterPanel::doUpdate(Renderer& renderer) { m_history.update(renderer); }
