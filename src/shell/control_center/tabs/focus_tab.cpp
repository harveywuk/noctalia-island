#include "shell/control_center/tabs/focus_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"

#include <charconv>
#include <format>
#include <sstream>

namespace {
  constexpr std::array<const char*, 4> kModes{"off", "work", "gaming", "sleep"};
  constexpr std::array<const char*, 7> kDays{"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  std::optional<int> minute(std::string_view value) {
    if (value.size() != 5 || value[2] != ':')
      return std::nullopt;
    int hour = 0, minute = 0;
    auto h = std::from_chars(value.data(), value.data() + 2, hour);
    auto m = std::from_chars(value.data() + 3, value.data() + 5, minute);
    if (h.ec != std::errc{}
        || m.ec != std::errc{}
        || h.ptr != value.data() + 2
        || m.ptr != value.data() + 5
        || hour < 0
        || hour > 23
        || minute < 0
        || minute > 59)
      return std::nullopt;
    return hour * 60 + minute;
  }
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

  auto recording = ui::column({.gap = scaled(8)});
  control_center::applySectionCardStyle(*recording, contentScale(), panelCardOpacity());
  recording->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = scaled(8)},
          ui::label({.text = i18n::tr("utilities.focus.while-recording"), .fontSize = scaled(13), .flexGrow = 1.0F}),
          ui::toggle(
              {.out = &m_recording,
               .checkedImmediate = m_config && m_config->config().notification.focus.whileRecording,
               .scale = contentScale(),
               .onChange = [this](bool enabled) {
                 if (m_config && !m_config->setOverride({"notification", "focus", "while_recording"}, enabled)) {
                   if (m_recording)
                     m_recording->setChecked(m_config->config().notification.focus.whileRecording);
                   if (m_feedback)
                     m_feedback->setText(i18n::tr("utilities.focus.save-failed"));
                 }
               }}
          )
      )
  );
  m_recording->setTabFocusKey("focus-while-recording");
  control_center::addBody(*recording, i18n::tr("utilities.focus.recording-detail"), contentScale());
  list->addChild(std::move(recording));

  auto editor = ui::column({.gap = scaled(10)});
  control_center::applySectionCardStyle(*editor, contentScale(), panelCardOpacity());
  control_center::addTitle(*editor, i18n::tr("utilities.focus.configure"), contentScale());
  std::vector<std::string> names;
  for (auto id : focus::kProfiles)
    names.push_back(i18n::tr("utilities.focus." + std::string(id)));
  editor->addChild(
      ui::select(
          {.options = names,
           .selectedIndex = m_editing,
           .fontSize = scaled(13),
           .onSelectionChanged = [this](std::size_t index, std::string_view) { loadProfile(index); }}
      )
  );
  control_center::addBody(*editor, i18n::tr("utilities.focus.allowed-apps"), contentScale());
  editor->addChild(
      ui::input({.out = &m_apps, .placeholder = i18n::tr("utilities.focus.apps-example"), .fontSize = scaled(13)})
  );
  control_center::addBody(*editor, i18n::tr("utilities.focus.apps-detail"), contentScale());
  editor->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = scaled(8)},
          ui::label({.text = i18n::tr("utilities.focus.allow-critical"), .fontSize = scaled(13), .flexGrow = 1.0F}),
          ui::toggle({.out = &m_critical, .scale = contentScale()})
      )
  );
  editor->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = scaled(8)},
          ui::label({.text = i18n::tr("utilities.focus.schedule"), .fontSize = scaled(13), .flexGrow = 1.0F}),
          ui::toggle({.out = &m_schedule, .scale = contentScale()})
      )
  );
  editor->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = scaled(8)},
          ui::label({.text = i18n::tr("utilities.focus.from"), .fontSize = scaled(12)}),
          ui::input({.out = &m_start, .placeholder = "09:00", .fontSize = scaled(13), .width = scaled(85)}),
          ui::label({.text = i18n::tr("utilities.focus.until"), .fontSize = scaled(12)}),
          ui::input({.out = &m_end, .placeholder = "17:00", .fontSize = scaled(13), .width = scaled(85)})
      )
  );
  auto days = ui::row({.gap = scaled(4)});
  for (std::size_t i = 0; i < kDays.size(); ++i)
    days->addChild(
        ui::button(
            {.out = &m_days[i],
             .text = i18n::tr(std::string("utilities.focus.days.") + kDays[i]),
             .fontSize = scaled(11),
             .paddingH = scaled(6),
             .flexGrow = 1.0F,
             .onClick = [this, i] {
               m_dayMask ^= 1 << i;
               m_days[i]->setVariant((m_dayMask & (1 << i)) ? ButtonVariant::Primary : ButtonVariant::Default);
             }}
        )
    );
  editor->addChild(std::move(days));
  control_center::addBody(*editor, i18n::tr("utilities.focus.schedule-detail"), contentScale());
  editor->addChild(
      ui::button(
          {.text = i18n::tr("utilities.focus.save"),
           .fontSize = scaled(13),
           .variant = ButtonVariant::Primary,
           .onClick = [this] { saveProfile(); }}
      )
  );
  editor->addChild(ui::label({.out = &m_feedback, .fontSize = scaled(12), .maxLines = 3}));
  list->addChild(std::move(editor));
  root->addChild(std::move(scroll));
  loadProfile(m_editing);
  syncStatus();
  return root;
}

void FocusTab::loadProfile(std::size_t index) {
  if (!m_config || !m_apps || index >= focus::kProfiles.size())
    return;
  m_editing = index;
  const auto* config = focus::profile(m_config->config().notification.focus, focus::kProfiles[index]);
  std::string apps;
  for (const auto& app : config->allowedApps) {
    if (!apps.empty())
      apps += ", ";
    apps += app;
  }
  m_apps->setValue(apps);
  m_start->setValue(std::format("{:02}:{:02}", config->startMinute / 60, config->startMinute % 60));
  m_end->setValue(std::format("{:02}:{:02}", config->endMinute / 60, config->endMinute % 60));
  m_start->setInvalid(false);
  m_end->setInvalid(false);
  m_schedule->setCheckedImmediate(config->scheduleEnabled);
  m_critical->setCheckedImmediate(config->allowCritical);
  m_dayMask = config->days;
  for (std::size_t i = 0; i < m_days.size(); ++i)
    m_days[i]->setVariant((m_dayMask & (1 << i)) ? ButtonVariant::Primary : ButtonVariant::Default);
  m_feedback->setText("");
}

void FocusTab::saveProfile() {
  if (!m_config || !m_apps)
    return;
  const auto start = minute(m_start->value()), end = minute(m_end->value());
  m_start->setInvalid(!start);
  m_end->setInvalid(!end);
  if (!start || !end || (m_schedule->checked() && (*start == *end || m_dayMask == 0))) {
    m_feedback->setText(i18n::tr("utilities.focus.invalid-schedule"));
    return;
  }
  std::vector<std::string> apps;
  std::istringstream stream(m_apps->value());
  for (std::string app; std::getline(stream, app, ',');) {
    app = StringUtils::trim(app);
    if (!app.empty() && !std::ranges::contains(apps, app))
      apps.push_back(std::move(app));
  }
  const auto field = [this](const char* key) {
    return std::vector<std::string>{"notification", "focus", std::string(focus::kProfiles[m_editing]), key};
  };
  const bool saved = m_config->setOverrides(
      {{field("allowed_apps"), apps},
       {field("allow_critical"), m_critical->checked()},
       {field("schedule_enabled"), m_schedule->checked()},
       {field("start_minute"), static_cast<std::int64_t>(*start)},
       {field("end_minute"), static_cast<std::int64_t>(*end)},
       {field("days"), static_cast<std::int64_t>(m_dayMask)}}
  );
  if (m_feedback)
    m_feedback->setText(i18n::tr(saved ? "utilities.focus.saved" : "utilities.focus.save-failed"));
}

void FocusTab::syncStatus() {
  if (m_recording && m_config && m_recording->checked() != m_config->config().notification.focus.whileRecording)
    m_recording->setChecked(m_config->config().notification.focus.whileRecording);
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
  m_status = m_feedback = nullptr;
  m_apps = m_start = m_end = nullptr;
  m_schedule = m_critical = nullptr;
  m_recording = nullptr;
  m_modes.fill(nullptr);
  m_days.fill(nullptr);
}
