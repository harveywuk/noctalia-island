#include "shell/control_center/tabs/focus_tab.h"

#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"

namespace {
  constexpr std::array<const char*, 4> kModes{"off", "work", "gaming", "sleep"};
} // namespace

std::unique_ptr<Flex> FocusTab::create() {
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});
  auto scroll = ui::scrollView({.contentScale = contentScale(), .flexGrow = 1.0F});
  scroll->clearFill();
  scroll->clearBorder();
  auto* list = scroll->content();
  list->setDirection(FlexDirection::Vertical);
  list->setAlign(FlexAlign::Stretch);
  list->setGap(scaled(12));
  auto status = ui::column({.gap = scaled(10)});
  control_center::applySectionCardStyle(*status, contentScale(), panelCardOpacity());
  status->addChild(ui::label({.out = &m_status, .fontSize = scaled(15), .fontWeight = FontWeight::SemiBold}));
  auto modes = ui::row({.gap = scaled(8)});
  for (std::size_t i = 0; i < kModes.size(); ++i)
    modes->addChild(
        ui::button(
            {.out = &m_modes[i],
             .text = i18n::tr(std::string("utilities.focus.") + kModes[i]),
             .fontSize = scaled(13),
             .flexGrow = 1.0F,
             .onClick = [this, i] {
               if (m_notifications)
                 m_notifications->selectFocus(kModes[i]);
               syncStatus();
             }}
        )
    );
  status->addChild(std::move(modes));
  status->addChild(
      ui::button({.text = i18n::tr("utilities.focus.follow-schedules"), .fontSize = scaled(12), .onClick = [this] {
                    if (m_notifications)
                      m_notifications->selectFocus("auto");
                    syncStatus();
                  }})
  );
  control_center::addBody(*status, i18n::tr("utilities.focus.manual-detail"), contentScale());
  list->addChild(std::move(status));

  root->addChild(std::move(scroll));
  syncStatus();
  return root;
}

void FocusTab::syncStatus() {
  if (!m_notifications || !m_status)
    return;
  const auto& id = m_notifications->focusId();
  const auto name = i18n::tr(
      id.empty() ? (m_notifications->doNotDisturb() ? "utilities.focus.dnd" : "utilities.focus.off")
                 : "utilities.focus." + id
  );
  m_status->setText(
      name
      + " · "
      + i18n::tr(m_notifications->focusAutomatic() ? "utilities.focus.automatic" : "utilities.focus.manual")
  );
  for (std::size_t i = 0; i < m_modes.size(); ++i)
    m_modes[i]->setVariant(
        (i == 0 ? id.empty() && !m_notifications->doNotDisturb() : id == kModes[i]) ? ButtonVariant::Primary
                                                                                    : ButtonVariant::Default
    );
}

void FocusTab::doLayout(Renderer& renderer, float width, float height) {
  if (!m_root)
    return;
  syncStatus();
  m_root->setSize(width, height);
  m_root->layout(renderer);
}
void FocusTab::doUpdate(Renderer&) { syncStatus(); }
void FocusTab::onClose() {
  m_root = nullptr;
  m_status = nullptr;
  m_modes.fill(nullptr);
}
