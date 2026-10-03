#include "shell/notification/notification_center_panel.h"

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

void NotificationCenterPanel::create() {
  const float scale = contentScale();
  m_history.setContentScale(scale);
  m_history.setPanelCardOpacity(panelCardOpacity());

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
  });

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::End,
  });
  header->addChild(m_history.createHeaderActions());
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
}

void NotificationCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr || m_header == nullptr || m_body == nullptr) {
    return;
  }
  m_root->setSize(width, height);
  m_root->layout(renderer);
  const float pad = Style::spaceSm * contentScale();
  const float bodyHeight = std::max(0.0F, height - pad * 2.0F - m_header->height() - m_root->gap());
  m_history.layout(renderer, std::max(0.0F, width - pad * 2.0F), bodyHeight);
  m_root->layout(renderer);
}

void NotificationCenterPanel::doUpdate(Renderer& renderer) { m_history.update(renderer); }
