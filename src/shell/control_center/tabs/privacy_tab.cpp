#include "shell/control_center/tabs/privacy_tab.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"

std::unique_ptr<Flex> PrivacyTab::create() {
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch, .gap = scaled(12)});
  auto scroll = ui::scrollView({.contentScale = contentScale(), .flexGrow = 1.0F});
  scroll->clearFill();
  scroll->clearBorder();
  m_list = scroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(scaled(12));
  root->addChild(std::move(scroll));
  rebuild();
  return root;
}

bool PrivacyTab::rebuild() {
  if (!m_list)
    return false;
  const auto state = m_audio ? m_audio->privacyState() : PrivacyState{};
  const auto filters = m_config ? m_config->config().shell.privacy : ShellConfig::PrivacyConfig{};
  if (m_built && state == m_state && filters == m_filters)
    return false;
  m_built = true;
  m_state = state;
  m_filters = filters;
  while (!m_list->children().empty())
    m_list->removeChild(m_list->children().back().get());
  const auto groups = m_summary.snapshot(state, filters);
  if (groups.empty()) {
    m_list->addChild(
        control_center::makeEmptyState(
            "shield-check", i18n::tr("utilities.privacy.empty"), i18n::tr("utilities.privacy.empty-detail"),
            contentScale(), panelCardOpacity()
        )
    );
  }
  for (const auto& group : groups) {
    auto card = ui::column({.gap = scaled(10)});
    control_center::applySectionCardStyle(*card, contentScale(), panelCardOpacity());
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = scaled(10)},
            ui::glyph({.glyph = group.icon(), .glyphSize = scaled(22), .color = colorSpecFromRole(ColorRole::Primary)}),
            ui::label({.text = i18n::tr(group.labelKey()), .fontSize = scaled(15), .fontWeight = FontWeight::SemiBold})
        )
    );
    for (const auto& app : group.apps) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = scaled(8)},
              ui::label({.text = app, .fontSize = scaled(13), .flexGrow = 1.0F}),
              ui::button(
                  {.text = i18n::tr("utilities.privacy.open-app"),
                   .fontSize = scaled(12),
                   .onClick = [this, kind = group.kind, app] { openApp(kind, app); }}
              )
          )
      );
    }
    m_list->addChild(std::move(card));
  }
  auto info = ui::column({});
  control_center::applySectionCardStyle(*info, contentScale(), panelCardOpacity());
  control_center::addBody(*info, i18n::tr("utilities.privacy.detail"), contentScale());
  info->addChild(ui::label({.out = &m_feedback, .fontSize = scaled(12), .maxLines = 2}));
  m_list->addChild(std::move(info));
  return true;
}

void PrivacyTab::openApp(PrivacyCaptureKind kind, const std::string& app) {
  if (!m_audio || !m_platform)
    return;
  for (const auto& capture : m_audio->privacyState().captures) {
    if (capture.kind != kind || capture.appName != app)
      continue;
    const auto identity = capture.binary.empty() ? capture.appName : capture.binary;
    const auto windows = m_platform->windowsForApp(identity, identity);
    if (windows.empty())
      continue;
    DeferredCall::callLater([platform = m_platform, window = windows.front()] {
      PanelManager::instance().closePanel(false);
      platform->activateToplevelInfo(window);
    });
    return;
  }
  // A background capture need not have an activatable window. Keep its details visible.
  if (m_feedback)
    m_feedback->setText(i18n::tr("utilities.privacy.no-window"));
  PanelManager::instance().refresh();
}

void PrivacyTab::doLayout(Renderer& renderer, float width, float height) {
  if (!m_root)
    return;
  rebuild();
  m_root->setSize(width, height);
  m_root->layout(renderer);
}

void PrivacyTab::doUpdate(Renderer& renderer) {
  if (rebuild() && m_root)
    m_root->layout(renderer);
}

void PrivacyTab::onClose() {
  m_root = m_list = nullptr;
  m_feedback = nullptr;
  m_built = false;
}
