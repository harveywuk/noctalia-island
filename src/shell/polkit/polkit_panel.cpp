#include "shell/polkit/polkit_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "dbus/polkit/polkit_agent.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>

namespace {

  int wrappedLineCount(std::string_view text, int charsPerLine, int maxLines) {
    if (text.empty()) {
      return 0;
    }
    int lines = 0;
    int col = 0;
    for (char ch : text) {
      if (ch == '\n') {
        ++lines;
        col = 0;
        if (lines >= maxLines) {
          return maxLines;
        }
        continue;
      }
      ++col;
      if (charsPerLine > 0 && col > charsPerLine) {
        ++lines;
        col = 1;
        if (lines >= maxLines) {
          return maxLines;
        }
      }
    }
    if (col > 0 || lines == 0) {
      ++lines;
    }
    return std::min(lines, maxLines);
  }

  std::string wrapLongRuns(std::string text, std::size_t maxRun = 48) {
    std::string out;
    out.reserve(text.size() + text.size() / maxRun);
    std::size_t run = 0;
    for (char ch : text) {
      const bool breakable = std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '/' || ch == ':' || ch == '-';
      out.push_back(ch);
      if (breakable) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= maxRun) {
        out.push_back('\n');
        run = 0;
      }
    }
    return out;
  }


  // What the alert shows, derived the same way for sizing and for display.
  struct AlertText {
    std::string title;
    std::string prompt;
    std::string supplementary;
    bool needsInput = false;
    bool promptIsError = false;
    bool invalidPassword = false;
  };

  bool isPlainPasswordPrompt(std::string_view prompt) {
    std::string lowered;
    for (char ch : prompt) {
      if (std::isspace(static_cast<unsigned char>(ch)) == 0 && ch != ':') {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
    }
    return lowered.empty() || lowered == "password";
  }

  AlertText alertText(PolkitAgent& agent) {
    AlertText out;
    const PolkitRequest request = agent.pendingRequest();
    out.needsInput = agent.isResponseRequired();
    out.title = wrapLongRuns(request.message.empty() ? i18n::tr("auth.polkit.title") : request.message);

    const std::string supplementaryRaw = agent.supplementaryMessage();
    const bool supplementaryError = agent.supplementaryIsError();
    out.invalidPassword = supplementaryError && supplementaryRaw == i18n::tr("auth.polkit.invalid-password");

    // polkit's own "Password:" prompt only repeats the field's placeholder, so the alert asks in
    // macOS's words instead; any other prompt (a PIN, a token) is shown as it is.
    const std::string rawPrompt = agent.inputPrompt();
    std::string promptText = out.needsInput && isPlainPasswordPrompt(rawPrompt) ? i18n::tr("auth.polkit.enter-password")
                                                                                : wrapLongRuns(rawPrompt);
    std::string supplementaryText = wrapLongRuns(supplementaryRaw);
    if (!out.needsInput && !supplementaryText.empty() && !supplementaryError) {
      promptText = supplementaryText;
      supplementaryText.clear();
    } else if (
        !supplementaryText.empty()
        && (supplementaryError || supplementaryText == i18n::tr("auth.polkit.authenticating"))
    ) {
      promptText = supplementaryText;
      out.promptIsError = supplementaryError;
      supplementaryText.clear();
    }
    out.prompt = std::move(promptText);
    out.supplementary = std::move(supplementaryText);
    return out;
  }

} // namespace

PolkitPanel::PolkitPanel(ConfigService* config, std::function<PolkitAgent*()> agentProvider)
    : m_config(config), m_agentProvider(std::move(agentProvider)) {}

PanelPlacement PolkitPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.polkitPlacement : PanelPlacement::Floating;
}

float PolkitPanel::preferredHeight() const {
  const float outer = scaled(Style::panelPadding) * 2.0F;
  if (m_measuredHeight > 0.0F) {
    return std::ceil(m_measuredHeight + outer);
  }

  // Before the first layout, estimate from the text so the alert opens close to its final size.
  const float scale = contentScale();
  const float titleLine = Style::fontSizeBody * scale * 1.35F;
  const float captionLine = Style::fontSizeCaption * scale * 1.35F;
  const float gap = Style::spaceMd * scale;
  const float gapSm = Style::spaceSm * scale;
  const float textW = scaled(kAlertWidth) - gapSm * 2.0F;
  const int titleChars = std::max(1, static_cast<int>(textW / (Style::fontSizeBody * scale * 0.58F)));
  const int captionChars = std::max(1, static_cast<int>(textW / (Style::fontSizeCaption * scale * 0.55F)));

  float height = gapSm * 2.0F + scaled(kIconSize) + gap + titleLine;
  if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
      agent != nullptr && agent->hasPendingRequest()) {
    const AlertText text = alertText(*agent);
    height += static_cast<float>(std::max(1, wrappedLineCount(text.title, titleChars, 4)) - 1) * titleLine;
    if (!text.prompt.empty()) {
      height += gapSm + static_cast<float>(wrappedLineCount(text.prompt, captionChars, 3)) * captionLine;
    }
    if (text.needsInput) {
      height += gap + Style::controlHeight * scale;
    }
    if (!text.supplementary.empty()) {
      height += gapSm + static_cast<float>(wrappedLineCount(text.supplementary, captionChars, 4)) * captionLine;
    }
  }
  height += gap + Style::controlHeight * scale;
  return std::ceil(height + outer);
}

void PolkitPanel::create() {
  const float scale = contentScale();
  const float iconSize = scaled(kIconSize);
  const float badgeSize = scaled(kBadgeSize);
  const float textWidth = scaled(kAlertWidth) - Style::spaceSm * scale * 2.0F;

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Center,
  });

  auto focusArea = ui::inputArea({});
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  focusArea->setParticipatesInLayout(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .paddingV = Style::spaceSm * scale,
      .width = textWidth,
  });

  // App icon with a padlock badge, as on macOS; a plain padlock when the app has no icon.
  auto iconContainer = ui::node({
      .out = &m_iconContainer,
      .width = iconSize,
      .height = iconSize,
  });
  auto well = ui::row({
      .out = &m_iconWell,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::OnSurface, Style::hoverFillAlpha * 1.5F),
      .radius = iconSize * 0.5F,
      .width = iconSize,
      .height = iconSize,
  });
  well->addChild(
      ui::glyph({
          .out = &m_fallbackIcon,
          .glyph = "lock-filled",
          .glyphSize = iconSize * 0.5F,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  iconContainer->addChild(std::move(well));
  iconContainer->addChild(
      ui::image({
          .out = &m_icon,
          .fit = ImageFit::Contain,
          .visible = false,
      })
  );
  auto badge = ui::row({
      .out = &m_lockBadge,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
      .radius = badgeSize * 0.5F,
      .border = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
      .width = badgeSize,
      .height = badgeSize,
      .visible = false,
  });
  badge->addChild(
      ui::glyph({
          .glyph = "lock-filled",
          .glyphSize = badgeSize * 0.55F,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  iconContainer->addChild(std::move(badge));
  content->addChild(std::move(iconContainer));

  auto text = ui::column({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = textWidth});
  text->addChild(
      ui::label({
          .out = &m_titleLabel,
          .text = i18n::tr("auth.polkit.title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = textWidth,
          .maxLines = 4,
          .textAlign = TextAlign::Center,
      })
  );
  text->addChild(
      ui::label({
          .out = &m_promptLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxWidth = textWidth,
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      })
  );
  content->addChild(std::move(text));

  auto fields = ui::column({
      .out = &m_fields,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .width = textWidth,
  });
  fields->addChild(
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("auth.polkit.password-placeholder"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange =
              [this](const std::string& value) {
                if (m_submitButton != nullptr) {
                  m_submitButton->setEnabled(m_lastResponseRequired && !value.empty());
                }
              },
          .onSubmit = [this](const std::string& value) { submit(value); },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) { return handleInputKeyEvent(sym, modifiers); },
      })
  );
  fields->addChild(
      ui::label({
          .out = &m_supplementaryLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxWidth = textWidth,
          .maxLines = 4,
          .textAlign = TextAlign::Center,
      })
  );

  content->addChild(std::move(fields));

  // Equal-width buttons, the default one on the right, like a Ventura alert.
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = textWidth});
  buttons->addChild(
      ui::button({
          .out = &m_cancelButton,
          .text = i18n::tr("common.actions.cancel"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .flexGrow = 1.0F,
          .onClick =
              [this]() {
                cancelAuth();
                PanelManager::instance().close();
              },
      })
  );
  buttons->addChild(
      ui::button({
          .out = &m_submitButton,
          .text = i18n::tr("auth.polkit.authenticate"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Primary,
          .flexGrow = 1.0F,
          .onClick = [this]() { submit(); },
      })
  );
  content->addChild(std::move(buttons));

  root->addChild(std::move(content));
  setRoot(std::move(root));
}

void PolkitPanel::onOpen(std::string_view /*context*/) {
  m_lastResponseRequired = false;
  m_lastInvalidPassword = false;
  m_iconResolved = false;
  m_hasTrackedRequest = false;
  m_measuredHeight = 0.0F;
  m_shakeOffset = 0.0F;
  m_trackedRequestCookie.clear();
  if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
      agent != nullptr && agent->hasPendingRequest()) {
    m_trackedRequestCookie = agent->pendingRequest().cookie;
    m_hasTrackedRequest = true;
  }
  if (m_input != nullptr) {
    m_input->setValue("");
  }
}

void PolkitPanel::onClose() {
  // A panel close must settle the request it displayed. Match the cookie so a
  // chained authentication that arrived after a successful response survives.
  if (m_hasTrackedRequest) {
    if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
        agent != nullptr && agent->hasPendingRequest() && agent->pendingRequest().cookie == m_trackedRequestCookie) {
      agent->cancelRequest();
    }
  }
  if (m_shakeAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_shakeAnimId);
  }
  m_shakeAnimId = 0;
  m_shakeOffset = 0.0F;
  m_hasTrackedRequest = false;
  m_trackedRequestCookie.clear();
  m_lastResponseRequired = false;
  m_lastInvalidPassword = false;
  m_measuredHeight = 0.0F;
  clearReleasedRoot();

  m_rootLayout = nullptr;
  m_content = nullptr;
  m_fields = nullptr;
  m_focusArea = nullptr;
  m_titleLabel = nullptr;
  m_promptLabel = nullptr;
  m_supplementaryLabel = nullptr;
  m_input = nullptr;
  m_submitButton = nullptr;
  m_cancelButton = nullptr;
  m_iconContainer = nullptr;
  m_iconWell = nullptr;
  m_lockBadge = nullptr;
  m_icon = nullptr;
  m_fallbackIcon = nullptr;
}

void PolkitPanel::cancelAuth() {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent != nullptr && agent->hasPendingRequest()) {
    agent->cancelRequest();
  }
}

bool PolkitPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  if (!pressed || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  cancelAuth();
  PanelManager::instance().close();
  return true;
}

InputArea* PolkitPanel::initialFocusArea() const {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent != nullptr && !agent->isResponseRequired()) {
    return m_focusArea;
  }
  return m_input != nullptr ? m_input->inputArea() : m_focusArea;
}

void PolkitPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  m_contentBaseX = m_content->x();
  applyShake();

  if (m_iconContainer != nullptr) {
    const float iconW = m_iconContainer->width();
    const float iconH = m_iconContainer->height();
    if (m_iconWell != nullptr) {
      m_iconWell->setPosition(0.0F, 0.0F);
      m_iconWell->layout(renderer);
    }
    if (m_icon != nullptr && m_icon->visible()) {
      m_icon->setSize(iconW, iconH);
      m_icon->setPosition(0.0F, 0.0F);
    }
    if (m_lockBadge != nullptr) {
      m_lockBadge->layout(renderer);
      // The badge sits on the icon's lower-right corner, slightly outside it, like macOS.
      const float overhang = scaled(4.0F);
      m_lockBadge->setPosition(
          std::round(iconW - m_lockBadge->width() + overhang), std::round(iconH - m_lockBadge->height() + overhang)
      );
    }
  }

  // Fit the panel to the alert's natural height once it is known, and whenever it changes.
  const float natural = m_content->height();
  if (natural > 0.0F && std::abs(natural - m_measuredHeight) > 0.5F) {
    m_measuredHeight = natural;
    if (std::abs(natural + scaled(Style::panelPadding) * 2.0F - height) > 0.5F
        && std::abs(preferredHeight() - height) > 0.5F) {
      DeferredCall::callLater([]() {
        if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("polkit")) {
          manager->relayoutActivePanelPreferredSize();
        }
      });
    }
  }
}

void PolkitPanel::doUpdate(Renderer& renderer) {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr
      || m_titleLabel == nullptr
      || m_promptLabel == nullptr
      || m_supplementaryLabel == nullptr
      || m_submitButton == nullptr
      || m_input == nullptr
      || m_icon == nullptr
      || m_fallbackIcon == nullptr) {
    return;
  }
  const PolkitRequest request = agent->pendingRequest();
  if (agent->hasPendingRequest()) {
    m_trackedRequestCookie = request.cookie;
    m_hasTrackedRequest = true;
  }
  const AlertText text = alertText(*agent);
  const bool needsInput = text.needsInput;

  m_titleLabel->setText(text.title);
  m_promptLabel->setText(text.prompt);
  m_promptLabel->setColor(
      text.promptIsError ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_promptLabel->setVisible(!text.prompt.empty());
  m_supplementaryLabel->setText(text.supplementary);
  m_supplementaryLabel->setVisible(!text.supplementary.empty());
  m_input->setVisible(needsInput);
  if (m_fields != nullptr) {
    m_fields->setVisible(needsInput || !text.supplementary.empty());
  }
  m_submitButton->setVisible(needsInput);
  m_submitButton->setEnabled(needsInput && !m_input->value().empty());
  if (needsInput != m_lastResponseRequired) {
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("polkit")) {
      manager->relayoutActivePanelPreferredSize();
      if (needsInput) {
        manager->focusArea(m_input->inputArea());
      }
    }
  }
  m_lastResponseRequired = needsInput;

  // macOS shakes the alert when the password is wrong.
  if (text.invalidPassword && !m_lastInvalidPassword) {
    startShake();
  }
  m_lastInvalidPassword = text.invalidPassword;

  if (request.iconName != m_lastIconName || !m_iconResolved) {
    m_lastIconName = request.iconName;
    m_iconResolved = true;
    resolveIcon(renderer, request);
  }
}

void PolkitPanel::resolveIcon(Renderer& renderer, const PolkitRequest& request) {
  const float iconSize = scaled(kIconSize);
  if (m_iconContainer != nullptr) {
    m_iconContainer->setSize(iconSize, iconSize);
  }
  const auto showAppIcon = [&](const std::string& path) {
    if (m_iconWell != nullptr) {
      m_iconWell->setVisible(false);
    }
    m_icon->setSize(iconSize, iconSize);
    m_icon->setSourceFile(renderer, path, static_cast<int>(std::round(iconSize)), true);
    m_icon->setVisible(true);
    if (m_lockBadge != nullptr) {
      m_lockBadge->setVisible(true);
    }
  };

  if (request.isInternal) {
    showAppIcon(paths::assetPath("noctalia.svg").string());
    return;
  }

  if (!request.iconName.empty()) {
    const std::string& resolved = m_iconResolver.resolve(request.iconName, static_cast<int>(std::round(iconSize)));
    if (!resolved.empty()) {
      showAppIcon(resolved);
      return;
    }
  }

  m_icon->clear(renderer);
  m_icon->setVisible(false);
  if (m_iconWell != nullptr) {
    m_iconWell->setVisible(true);
  }
  if (m_lockBadge != nullptr) {
    m_lockBadge->setVisible(false);
  }
}

void PolkitPanel::startShake() {
  if (m_animations == nullptr || m_content == nullptr) {
    return;
  }
  if (m_shakeAnimId != 0) {
    m_animations->cancel(m_shakeAnimId);
  }
  // Three quick swings that die away, about as far and as fast as the macOS login shake.
  const float amplitude = scaled(10.0F);
  m_shakeAnimId = m_animations->animate(
      0.0F, 1.0F, 420.0F, Easing::Linear,
      [this, amplitude](float t) {
        m_shakeOffset = amplitude * std::sin(t * 6.0F * std::numbers::pi_v<float>) * (1.0F - t);
        applyShake();
      },
      [this]() {
        m_shakeAnimId = 0;
        m_shakeOffset = 0.0F;
        applyShake();
      },
      m_content
  );
}

void PolkitPanel::applyShake() {
  if (m_content == nullptr) {
    return;
  }
  m_content->setPosition(std::round(m_contentBaseX + m_shakeOffset), m_content->y());
  m_content->markPaintDirty();
}

void PolkitPanel::submit(std::string_view response) {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr || m_input == nullptr) {
    return;
  }
  const std::string password = response.empty() ? m_input->value() : std::string(response);
  if (password.empty()) {
    return;
  }
  agent->submitResponse(password);
  m_input->setValue("");
}

bool PolkitPanel::handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    submit();
    return true;
  }
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretLeft(shift);
    }
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretRight(shift);
    }
    return true;
  }
  return false;
}

void PolkitPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
}
