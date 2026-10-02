#include "shell/session/session_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "system/distro_info.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <linux/input-event-codes.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("session");

  // Menu metrics follow ContextMenuControl so the session menu reads like every other
  // shell menu (and like a macOS menu): body-size items, a thin rule between groups.
  constexpr float kRowHeight = Style::controlHeightSm;
  constexpr float kSeparatorHeight = 10.0F;
  constexpr float kRowInset = 8.0F;

  // Internal open contexts, used when the panel has to reopen itself to change size
  // (attached panels can't resize in place).
  constexpr std::string_view kConfirmContextPrefix = "@confirm:";
  constexpr std::string_view kMenuContextPrefix = "@menu:";

  [[nodiscard]] ButtonVariant buttonVariantFor(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
      return ButtonVariant::Default;
    case SessionActionButtonVariant::Primary:
      return ButtonVariant::Primary;
    case SessionActionButtonVariant::Secondary:
      return ButtonVariant::Secondary;
    case SessionActionButtonVariant::Destructive:
      return ButtonVariant::Destructive;
    case SessionActionButtonVariant::Outline:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

  // macOS groups the power actions (Sleep, Restart, Shut Down) apart from the session
  // ones (Lock Screen, Log Out); a rule is drawn wherever the group changes.
  [[nodiscard]] int actionGroup(std::string_view action) {
    if (action == "suspend" || action == "lock_and_suspend" || action == "reboot" || action == "shutdown") {
      return 0;
    }
    if (action == "lock" || action == "logout") {
      return 1;
    }
    return 2;
  }

  // Translation key segment for the alert's wording.
  [[nodiscard]] std::string_view confirmKey(std::string_view action) {
    if (action == "shutdown" || action == "reboot" || action == "logout" || action == "lock") {
      return action;
    }
    if (action == "suspend" || action == "lock_and_suspend") {
      return "suspend";
    }
    return "custom";
  }

  [[nodiscard]] std::optional<std::size_t> parseIndex(std::string_view text) {
    std::size_t value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
      return std::nullopt;
    }
    return value;
  }

} // namespace

std::vector<SessionPanelActionConfig> SessionPanel::effectiveActions() const {
  std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();

  std::vector<SessionPanelActionConfig> out;
  out.reserve(src.size());
  for (const auto& row : src) {
    if (!row.enabled) {
      continue;
    }
    if (!session_action::isKnown(row.action)) {
      kLog.warn("session panel: skipping unknown action \"{}\"", row.action);
      continue;
    }
    if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
      kLog.warn("session panel: skipping \"command\" entry with no command");
      continue;
    }
    if ((row.action == "lock" || row.action == "lock_and_suspend")
        && m_config != nullptr
        && !m_config->isLockScreenEnabled()) {
      continue;
    }
    out.push_back(row);
  }
  return out;
}

std::string SessionPanel::actionLabel(const SessionPanelActionConfig& cfg) const {
  return cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(session_action::labelKey(cfg.action));
}

std::string SessionPanel::menuLabel(const SessionPanelActionConfig& cfg) const {
  std::string text;
  if (cfg.action == "logout" && (!cfg.label.has_value() || cfg.label->empty())) {
    // "Log Out Harvey…", as in the Apple menu.
    const std::string name = sessionDisplayName();
    text = name.empty() ? actionLabel(cfg) : i18n::tr("session.menu.logout-user", "name", name);
  } else {
    text = actionLabel(cfg);
  }
  // macOS marks menu items that open a confirmation with an ellipsis.
  if (needsConfirm(cfg) && !text.ends_with("…") && !text.ends_with("...")) {
    text += "…";
  }
  return text;
}

bool SessionPanel::needsConfirm(const SessionPanelActionConfig& cfg) const { return cfg.countdownSeconds > 0.0; }

PanelPlacement SessionPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.sessionPlacement : PanelPlacement::Attached;
}

float SessionPanel::preferredWidth() const { return std::round(scaled(kContentWidth + Style::panelPadding * 2.0F)); }

float SessionPanel::menuHeight() const {
  const std::vector<SessionPanelActionConfig> entries = !m_visibleEntries.empty() ? m_visibleEntries : effectiveActions();
  std::size_t separators = 0;
  for (std::size_t i = 1; i < entries.size(); ++i) {
    if (actionGroup(entries[i].action) != actionGroup(entries[i - 1].action)) {
      ++separators;
    }
  }
  return kRowHeight * static_cast<float>(std::max<std::size_t>(1, entries.size()))
      + kSeparatorHeight * static_cast<float>(separators);
}

SessionPanel::Stage SessionPanel::stageForContext(std::string_view context) const {
  if (context.starts_with(kConfirmContextPrefix)) {
    return Stage::Confirm;
  }
  if (context.empty() || context.starts_with(kMenuContextPrefix)) {
    return Stage::Menu;
  }
  const auto action = session_action::canonicalActionName(context);
  if (!action.has_value()) {
    return Stage::Menu;
  }
  for (const auto& cfg : !m_visibleEntries.empty() ? m_visibleEntries : effectiveActions()) {
    if (cfg.action == *action) {
      return needsConfirm(cfg) ? Stage::Confirm : Stage::Menu;
    }
  }
  return Stage::Menu;
}

float SessionPanel::alertHeightEstimate() const {
  // Icon, a two-line bold title, a two-line caption and the button row.
  const float titleLines = 2.0F * Style::fontSizeBody * 1.35F;
  const float bodyLines = 2.0F * Style::fontSizeCaption * 1.35F;
  return Style::spaceXs + kAlertIconSize + Style::spaceMd + titleLines + Style::spaceXs + bodyLines + Style::spaceLg
      + Style::controlHeightSm;
}

float SessionPanel::preferredHeight() const {
  // Before create() the host is sizing a fresh open, so the open context decides the stage.
  const Stage stage = m_rootNode == nullptr ? stageForContext(pendingOpenContext()) : m_stage;
  m_sizedStage = stage;
  float content = 0.0F;
  if (stage == Stage::Confirm) {
    content = m_rootNode != nullptr && m_measuredAlertHeight > 0.0F
        ? m_measuredAlertHeight / std::max(0.1F, contentScale())
        : alertHeightEstimate();
  } else {
    content = menuHeight();
  }
  return std::ceil(scaled(content + Style::panelPadding * 2.0F));
}

void SessionPanel::create() {
  const float scale = contentScale();
  m_visibleEntries = effectiveActions();
  m_rows.clear();
  m_separators.clear();
  m_separatorRows.clear();
  m_measuredAlertHeight = 0.0F;

  auto root = std::make_unique<Node>();
  m_rootNode = root.get();
  buildMenu(*root, scale);
  buildAlert(*root, scale);
  setRoot(std::move(root));

  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }
  applyStage();
}

void SessionPanel::buildMenu(Node& parent, float scale) {
  auto menu = std::make_unique<Node>();
  m_menuNode = menu.get();

  const bool showShortcuts = m_config == nullptr || m_config->config().shell.session.showShortcuts;
  const float highlightRadius = std::max(0.0F, Style::scaledRadiusLg(scale) - Style::spaceXs * scale);

  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const SessionPanelActionConfig& cfg = m_visibleEntries[i];
    if (i > 0 && actionGroup(cfg.action) != actionGroup(m_visibleEntries[i - 1].action)) {
      auto sep = ui::separator({
          .orientation = SeparatorOrientation::HorizontalRule,
          .configure = [scale](Separator& s) { s.setThickness(std::max(1.0F, scale)); },
      });
      m_separators.push_back(sep.get());
      m_separatorRows.push_back(m_rows.size());
      menu->addChild(std::move(sep));
    }

    MenuRow row{.entryIndex = i};
    const std::size_t rowIndex = m_rows.size();
    auto area = ui::inputArea({.out = &row.area});
    area->addChild(ui::box({.out = &row.highlight, .fill = clearColorSpec(), .radius = highlightRadius}));
    area->addChild(
        ui::label({
            .out = &row.label,
            .text = menuLabel(cfg),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    if (showShortcuts && cfg.shortcut.has_value() && cfg.shortcut->sym != 0) {
      area->addChild(
          ui::label({
              .out = &row.shortcut,
              .text = keyChordDisplayLabel(*cfg.shortcut),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      );
    }
    area->setOnEnter([this, rowIndex](const InputArea::PointerData& /*data*/) { setHighlightedRow(rowIndex); });
    area->setOnLeave([this, rowIndex]() {
      if (m_highlightedRow == rowIndex) {
        setHighlightedRow(std::nullopt);
      }
    });
    area->setOnClick([this, i](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        activateEntry(i);
      }
    });
    m_rows.push_back(row);
    menu->addChild(std::move(area));
  }

  parent.addChild(std::move(menu));
}

void SessionPanel::buildAlert(Node& parent, float scale) {
  const float width = kContentWidth * scale;

  auto alert = std::make_unique<Node>();
  m_alertNode = alert.get();
  alert->addChild(
      ui::glyph({
          .out = &m_alertIcon,
          .glyph = "shutdown",
          .glyphSize = kAlertIconSize * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  alert->addChild(
      ui::label({
          .out = &m_alertTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = width,
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      })
  );
  alert->addChild(
      ui::label({
          .out = &m_alertBody,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = width,
          .maxLines = 4,
          .textAlign = TextAlign::Center,
      })
  );

  // Cancel on the left, the default (blue) action on the right, sharing the width
  // equally, as in a two-button Ventura alert. layoutAlert() places them.
  const auto alertButton = [this, scale](Button** out, std::string text, std::function<void()> onClick) {
    return ui::button({
        .out = out,
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .onClick = std::move(onClick),
        .configure = [](Button& b) { b.setTabStop(false); },
    });
  };
  alert->addChild(alertButton(&m_cancelButton, i18n::tr("session.confirm.cancel"), [this]() { cancelConfirm(); }));
  alert->addChild(alertButton(&m_confirmButton, "", [this]() {
    if (m_pendingConfirm.has_value()) {
      executeEntry(m_pendingConfirm->index);
    }
  }));

  parent.addChild(std::move(alert));
}

void SessionPanel::onPanelCardOpacityChanged(float opacity) {
  for (Button* button : {m_cancelButton, m_confirmButton}) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
}

std::optional<std::size_t> SessionPanel::rowForEntry(std::size_t entryIndex) const {
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if (m_rows[i].entryIndex == entryIndex) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> SessionPanel::entryForContext(std::string_view context) const {
  const auto action = session_action::canonicalActionName(context);
  if (!action.has_value()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    if (m_visibleEntries[i].action == *action) {
      return i;
    }
  }
  return std::nullopt;
}

void SessionPanel::onOpen(std::string_view context) {
  m_actionQueued = false;
  m_pendingConfirm.reset();
  m_confirmOpenedDirectly = false;
  m_stage = Stage::Menu;
  m_highlightedRow.reset();

  if (context.starts_with(kConfirmContextPrefix)) {
    // Reopened by showConfirm() on an attached panel.
    if (const auto index = parseIndex(context.substr(kConfirmContextPrefix.size()));
        index.has_value() && *index < m_visibleEntries.size()) {
      m_stage = Stage::Confirm;
      m_pendingConfirm = PendingConfirm{
          .index = *index,
          .remainingMs = m_visibleEntries[*index].countdownSeconds * 1000.0,
      };
      m_confirmFocused = true;
    }
  } else if (context.starts_with(kMenuContextPrefix)) {
    if (const auto index = parseIndex(context.substr(kMenuContextPrefix.size())); index.has_value()) {
      m_highlightedRow = rowForEntry(*index);
    }
  } else if (const auto index = entryForContext(context); index.has_value()) {
    // `noctalia msg panel-open session shutdown` goes straight to the alert, like the
    // power key on a Mac; actions without a countdown are just highlighted.
    if (needsConfirm(m_visibleEntries[*index])) {
      m_stage = Stage::Confirm;
      m_pendingConfirm = PendingConfirm{
          .index = *index,
          .remainingMs = m_visibleEntries[*index].countdownSeconds * 1000.0,
      };
      m_confirmOpenedDirectly = true;
      m_confirmFocused = true;
    } else {
      m_highlightedRow = rowForEntry(*index);
    }
  }

  m_reopenContext = std::string(context);
  applyStage();
  if (m_stage == Stage::Confirm) {
    PanelManager::instance().requestFrameTick();
  }
}

void SessionPanel::applyStage() {
  const bool confirm = m_stage == Stage::Confirm;
  if (m_menuNode != nullptr) {
    m_menuNode->setVisible(!confirm);
  }
  if (m_alertNode != nullptr) {
    m_alertNode->setVisible(confirm);
  }
  if (confirm) {
    updateAlertText();
    setAlertFocus(m_confirmFocused);
  }
  setHighlightedRow(confirm ? std::nullopt : m_highlightedRow);
  if (m_rootNode != nullptr) {
    m_rootNode->markLayoutDirty();
  }
}

void SessionPanel::requestHostResize() {
  if (m_resizeQueued) {
    return;
  }
  m_resizeQueued = true;
  // Deferred: this runs from layout, and reopening an attached panel destroys its scene.
  DeferredCall::callLater([this]() {
    m_resizeQueued = false;
    PanelManager& manager = PanelManager::instance();
    if (!manager.isOpenPanel("session") || m_rootNode == nullptr || m_stage == m_sizedStage) {
      return;
    }
    if (manager.isAttachedOpen()) {
      // An attached panel's geometry is fixed when it opens, so reopen it at the new
      // size on the same bar.
      const std::string context = m_reopenContext;
      const std::string bar(manager.attachedSourceBarName());
      manager.openPanel("session", PanelOpenRequest{.output = manager.attachedPanelOutput(), .context = context, .sourceBarName = bar});
      return;
    }
    manager.relayoutActivePanelPreferredSize();
    manager.requestLayout();
    manager.refresh();
  });
}

void SessionPanel::activateEntry(std::size_t index) {
  if (index >= m_visibleEntries.size() || m_actionQueued) {
    return;
  }
  if (needsConfirm(m_visibleEntries[index])) {
    showConfirm(index);
    return;
  }
  executeEntry(index);
}

void SessionPanel::showConfirm(std::size_t index) {
  m_stage = Stage::Confirm;
  m_confirmOpenedDirectly = false;
  m_confirmFocused = true;
  m_measuredAlertHeight = 0.0F;
  m_pendingConfirm = PendingConfirm{
      .index = index,
      .remainingMs = m_visibleEntries[index].countdownSeconds * 1000.0,
  };
  m_reopenContext = std::string(kConfirmContextPrefix) + std::to_string(index);
  applyStage();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().refresh();
}

void SessionPanel::showMenu(std::optional<std::size_t> highlight) {
  m_stage = Stage::Menu;
  m_pendingConfirm.reset();
  m_highlightedRow = highlight;
  const std::size_t entry = highlight.has_value() && *highlight < m_rows.size() ? m_rows[*highlight].entryIndex : 0;
  m_reopenContext = std::string(kMenuContextPrefix) + std::to_string(entry);
  applyStage();
  PanelManager::instance().requestLayout();
  PanelManager::instance().refresh();
}

void SessionPanel::cancelConfirm() {
  if (!m_pendingConfirm.has_value()) {
    return;
  }
  if (m_confirmOpenedDirectly) {
    m_pendingConfirm.reset();
    DeferredCall::callLater([]() { PanelManager::instance().closePanel(); });
    return;
  }
  showMenu(rowForEntry(m_pendingConfirm->index));
}

void SessionPanel::executeEntry(std::size_t index) {
  if (index >= m_visibleEntries.size() || m_actionQueued) {
    return;
  }
  SessionActionRunner* const actionRunner = m_actionRunner;
  if (actionRunner == nullptr) {
    kLog.warn("session panel: action runner unavailable");
    return;
  }

  const SessionPanelActionConfig cfg = m_visibleEntries[index];
  m_actionQueued = true;
  m_pendingConfirm.reset();
  DeferredCall::callLater([actionRunner, cfg]() {
    PanelManager::instance().closePanel(/*animateClose=*/false);
    actionRunner->invoke(cfg);
  });
}

void SessionPanel::updateAlertText() {
  if (!m_pendingConfirm.has_value() || m_pendingConfirm->index >= m_visibleEntries.size()) {
    return;
  }
  const SessionPanelActionConfig& cfg = m_visibleEntries[m_pendingConfirm->index];
  const int seconds = std::max(1, static_cast<int>(std::ceil(m_pendingConfirm->remainingMs / 1000.0)));
  if (seconds == m_pendingConfirm->shownSeconds) {
    return;
  }
  m_pendingConfirm->shownSeconds = seconds;

  const std::string key = std::string("session.confirm.") + std::string(confirmKey(cfg.action));
  const std::string label = actionLabel(cfg);
  if (m_alertIcon != nullptr) {
    (void)m_alertIcon->setGlyph(
        cfg.glyph.has_value() && !cfg.glyph->empty() ? *cfg.glyph : session_action::defaultGlyph(cfg.action)
    );
  }
  if (m_alertTitle != nullptr) {
    (void)m_alertTitle->setText(i18n::tr(key + "-title", "label", label));
  }
  if (m_alertBody != nullptr) {
    (void)m_alertBody->setText(i18n::trp(key + "-body", seconds, "label", label));
  }
  if (m_confirmButton != nullptr) {
    m_confirmButton->setText(label);
  }
}

void SessionPanel::setHighlightedRow(std::optional<std::size_t> row) {
  if (row.has_value() && *row >= m_rows.size()) {
    row.reset();
  }
  m_highlightedRow = row;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    const MenuRow& r = m_rows[i];
    const bool on = row.has_value() && *row == i;
    if (r.highlight != nullptr) {
      r.highlight->setFill(on ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec());
    }
    if (r.label != nullptr) {
      r.label->setColor(on ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
    }
    if (r.shortcut != nullptr) {
      r.shortcut->setColor(
          on ? colorSpecFromRole(ColorRole::OnPrimary, 0.8F) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
    }
  }
  requestRedraw();
}

void SessionPanel::setAlertFocus(bool confirmFocused) {
  // Whichever button Return will press is drawn as the blue default button, so moving
  // focus with Tab or the arrows always shows what happens next.
  m_confirmFocused = confirmFocused;
  if (m_cancelButton != nullptr) {
    m_cancelButton->setVariant(confirmFocused ? ButtonVariant::Default : ButtonVariant::Primary);
  }
  if (m_confirmButton != nullptr) {
    ButtonVariant variant = ButtonVariant::Default;
    if (confirmFocused) {
      const SessionActionButtonVariant configured =
          m_pendingConfirm.has_value() && m_pendingConfirm->index < m_visibleEntries.size()
          ? m_visibleEntries[m_pendingConfirm->index].variant
          : SessionActionButtonVariant::Default;
      variant = configured == SessionActionButtonVariant::Default ? ButtonVariant::Primary : buttonVariantFor(configured);
    }
    m_confirmButton->setVariant(variant);
  }
  requestRedraw();
}

void SessionPanel::requestRedraw() {
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
}

void SessionPanel::onFrameTick(float deltaMs) {
  if (!m_pendingConfirm.has_value() || m_stage != Stage::Confirm || m_actionQueued) {
    return;
  }

  m_pendingConfirm->remainingMs -= static_cast<double>(deltaMs);
  if (m_pendingConfirm->remainingMs <= 0.0) {
    executeEntry(m_pendingConfirm->index);
    return;
  }

  const int before = m_pendingConfirm->shownSeconds;
  updateAlertText();
  if (m_pendingConfirm.has_value() && m_pendingConfirm->shownSeconds != before) {
    PanelManager::instance().requestLayout();
    PanelManager::instance().refresh();
  }
  PanelManager::instance().requestFrameTick();
}

bool SessionPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  if (m_stage == Stage::Confirm) {
    if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
      cancelConfirm();
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::TabPrevious, sym, modifiers)) {
      setAlertFocus(!m_confirmFocused);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
      if (m_confirmFocused && m_pendingConfirm.has_value()) {
        executeEntry(m_pendingConfirm->index);
      } else {
        cancelConfirm();
      }
      return true;
    }
    // Pressing the action's own shortcut again confirms at once.
    if (m_pendingConfirm.has_value()) {
      const auto& cfg = m_visibleEntries[m_pendingConfirm->index];
      if (cfg.shortcut.has_value() && keyChordMatches(*cfg.shortcut, sym, modifiers)) {
        executeEntry(m_pendingConfirm->index);
        return true;
      }
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    if (m_highlightedRow.has_value()) {
      setHighlightedRow(std::nullopt);
      return true;
    }
    return false;
  }

  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& cfg = m_visibleEntries[i];
    if (cfg.shortcut.has_value() && keyChordMatches(*cfg.shortcut, sym, modifiers)) {
      activateEntry(i);
      return true;
    }
  }

  if (m_rows.empty()) {
    return false;
  }
  const std::size_t last = m_rows.size() - 1;

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)
      || KeybindMatcher::matches(KeybindAction::TabPrevious, sym, modifiers)) {
    // Like a macOS menu, moving past either end wraps around.
    setHighlightedRow(!m_highlightedRow.has_value() || *m_highlightedRow == 0 ? last : *m_highlightedRow - 1);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)
      || KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers)) {
    setHighlightedRow(!m_highlightedRow.has_value() || *m_highlightedRow >= last ? 0 : *m_highlightedRow + 1);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (!m_highlightedRow.has_value()) {
      return false;
    }
    activateEntry(m_rows[*m_highlightedRow].entryIndex);
    return true;
  }

  return false;
}

void SessionPanel::layoutMenu(Renderer& renderer, float width) {
  if (m_menuNode == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float rowHeight = kRowHeight * scale;
  const float separatorHeight = kSeparatorHeight * scale;
  const float inset = kRowInset * scale;

  float y = 0.0F;
  std::size_t nextSeparator = 0;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    while (nextSeparator < m_separatorRows.size() && m_separatorRows[nextSeparator] == i) {
      if (Separator* sep = m_separators[nextSeparator]; sep != nullptr) {
        const float thickness = std::max(1.0F, scale);
        sep->setPosition(inset, y + std::round((separatorHeight - thickness) * 0.5F));
        sep->setFrameSize(width - inset * 2.0F, thickness);
        sep->layout(renderer);
      }
      y += separatorHeight;
      ++nextSeparator;
    }

    const MenuRow& row = m_rows[i];
    if (row.area == nullptr) {
      continue;
    }
    row.area->setPosition(0.0F, y);
    row.area->setFrameSize(width, rowHeight);
    if (row.highlight != nullptr) {
      row.highlight->setPosition(0.0F, 0.0F);
      row.highlight->setFrameSize(width, rowHeight);
      row.highlight->setSize(width, rowHeight);
    }
    float shortcutWidth = 0.0F;
    if (row.shortcut != nullptr) {
      row.shortcut->measure(renderer);
      shortcutWidth = row.shortcut->width();
      row.shortcut->setPosition(width - inset - shortcutWidth, std::round((rowHeight - row.shortcut->height()) * 0.5F));
    }
    if (row.label != nullptr) {
      row.label->setMaxWidth(std::max(1.0F, width - inset * 2.0F - (shortcutWidth > 0.0F ? shortcutWidth + inset : 0.0F)));
      row.label->measure(renderer);
      row.label->setPosition(inset, std::round((rowHeight - row.label->height()) * 0.5F));
    }
    y += rowHeight;
  }
  m_menuNode->setPosition(0.0F, 0.0F);
  m_menuNode->setFrameSize(width, y);
}

void SessionPanel::layoutAlert(Renderer& renderer, float width) {
  if (m_alertNode == nullptr || m_alertIcon == nullptr || m_alertTitle == nullptr || m_alertBody == nullptr) {
    return;
  }
  const float scale = contentScale();
  m_alertTitle->setMaxWidth(width);
  m_alertBody->setMaxWidth(width);
  m_alertIcon->measure(renderer);
  m_alertTitle->measure(renderer);
  m_alertBody->measure(renderer);

  // Ventura's alert: icon, then the bold question, then the countdown in small type,
  // with the buttons spanning the bottom.
  float y = Style::spaceXs * scale;
  m_alertIcon->setPosition(std::round((width - m_alertIcon->width()) * 0.5F), y);
  y += m_alertIcon->height() + Style::spaceMd * scale;
  m_alertTitle->setPosition(std::round((width - m_alertTitle->width()) * 0.5F), y);
  y += m_alertTitle->height() + Style::spaceXs * scale;
  m_alertBody->setPosition(std::round((width - m_alertBody->width()) * 0.5F), y);
  y += m_alertBody->height() + Style::spaceLg * scale;

  const float buttonHeight = Style::controlHeightSm * scale;
  const float gap = Style::spaceSm * scale;
  const float buttonWidth = std::floor((width - gap) * 0.5F);
  if (m_cancelButton != nullptr) {
    m_cancelButton->setPosition(0.0F, y);
    m_cancelButton->setSize(buttonWidth, buttonHeight);
    m_cancelButton->layout(renderer);
    m_cancelButton->updateInputArea();
  }
  if (m_confirmButton != nullptr) {
    m_confirmButton->setPosition(width - buttonWidth, y);
    m_confirmButton->setSize(buttonWidth, buttonHeight);
    m_confirmButton->layout(renderer);
    m_confirmButton->updateInputArea();
  }
  y += buttonHeight;

  m_alertNode->setPosition(0.0F, 0.0F);
  m_alertNode->setFrameSize(width, y);

  // Follow the measured text: the countdown or a translation can change the line count.
  if (std::abs(y - m_measuredAlertHeight) >= 1.0F) {
    m_measuredAlertHeight = y;
    DeferredCall::callLater([]() {
      PanelManager& manager = PanelManager::instance();
      if (manager.isOpenPanel("session") && !manager.isAttachedOpen()) {
        manager.relayoutActivePanelPreferredSize();
      }
    });
  }
}

void SessionPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootNode == nullptr) {
    return;
  }
  m_rootNode->setPosition(0.0F, 0.0F);
  m_rootNode->setFrameSize(width, height);
  if (m_stage == Stage::Confirm) {
    layoutAlert(renderer, width);
  } else {
    layoutMenu(renderer, width);
  }
  (void)height;
  if (m_stage != m_sizedStage) {
    requestHostResize();
  }
}

void SessionPanel::onClose() {
  m_pendingConfirm.reset();
  m_highlightedRow.reset();
  m_stage = Stage::Menu;
  m_rootNode = nullptr;
  m_menuNode = nullptr;
  m_alertNode = nullptr;
  m_alertIcon = nullptr;
  m_alertTitle = nullptr;
  m_alertBody = nullptr;
  m_cancelButton = nullptr;
  m_confirmButton = nullptr;
  m_rows.clear();
  m_separators.clear();
  m_separatorRows.clear();
  m_visibleEntries.clear();
  m_measuredAlertHeight = 0.0F;
  m_reopenContext.clear();
  clearReleasedRoot();
}
