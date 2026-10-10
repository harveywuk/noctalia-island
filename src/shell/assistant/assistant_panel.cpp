#include "shell/assistant/assistant_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/animation/motion_service.h"
#include "shell/assistant/assistant_orb.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/markdown_view.h"
#include "ui/visuals/audio_visualizer.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::array<Color, 4> kColors{hex("#67d9ff"), hex("#8594ff"), hex("#d88bff"), hex("#ffa8bd")};

  void quiet(Button& button) {
    auto palette = Button::defaultPalette(ButtonVariant::Ghost);
    for (auto* state : {&palette.normal, &palette.hover, &palette.pressed}) {
      state->bg = clearColorSpec();
      state->border = clearColorSpec();
    }
    palette.pressed.label = colorSpecFromRole(ColorRole::OnSurface, 0.55F);
    button.setCustomPalette(palette);
  }
} // namespace

AssistantPanel::AssistantPanel(
    ConfigService* config, ClipboardService* clipboard, HttpClient* http, MprisService* mpris,
    PipeWireSpectrum* spectrum, IpcService* ipc, PipeWireService* audio, UPowerService* power
)
    : m_ai(clipboard, config, http), m_localActions(ipc, audio, mpris, power), m_ducking(audio, mpris),
      m_config(config), m_clipboard(clipboard), m_http(http), m_mpris(mpris), m_spectrum(spectrum) {
  m_ai.setResultsChangedCallback([this] { sync(); });
  m_voice.setChanged([this] { sync(); });
  m_voice.setTranscript([this](std::string text) {
    if (!m_open || !m_input)
      return;
    if (!m_draft.empty())
      m_draft += " ";
    m_draft += text;
    m_dictated = true;
    m_input->setEnabled(true);
    m_input->setValue(m_draft);
    PanelManager::instance().focusArea(m_input->inputArea());
    sync();
  });
}

AssistantPanel::~AssistantPanel() {
  m_ai.setResultsChangedCallback({});
  m_voice.setChanged({});
  m_voice.setTranscript({});
  m_alive.reset();
  onClose();
}

bool AssistantPanel::compact() const { return PanelManager::instance().islandCompactLayout(); }
float AssistantPanel::preferredWidth() const { return scaled(compact() ? 392.0F : 504.0F); }
float AssistantPanel::preferredHeight() const { return scaled(156); }
float AssistantPanel::fittedHeight() const { return m_fittedHeight; }
float AssistantPanel::islandHeight(float availableHeight) const { return std::min(m_fittedHeight, availableHeight); }
std::optional<std::array<Color, 4>> AssistantPanel::islandAccentColors() const {
  return m_orb ? m_orb->style().orbFace.colors : kColors;
}
InputArea* AssistantPanel::initialFocusArea() const { return m_input ? m_input->inputArea() : nullptr; }

void AssistantPanel::create() {
  const float s = contentScale();
  auto root = std::make_unique<Node>();
  auto header = std::make_unique<Node>();
  m_header = header.get();
  auto orb = std::make_unique<AssistantOrb>();
  m_orb = orb.get();
  header->addChild(std::move(orb));
  auto activity =
      ui::row({.out = &m_activity, .align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 8 * s});
  activity->addChild(ui::label({.out = &m_activityLabel, .fontSize = 14 * s, .maxLines = 1}));
  activity->addChild(
      ui::button(
          {.out = &m_finishDictation,
           .glyph = "check",
           .glyphSize = 18 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.voice.finish"),
           .onClick = [this] { m_voice.finishRecording(); },
           .configure = quiet}
      )
  );
  header->addChild(std::move(activity));
  header->addChild(
      ui::button(
          {.out = &m_close,
           .glyph = "x",
           .glyphSize = 18 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.close"),
           .onClick = [] { PanelManager::instance().closePanelById("assistant"); },
           .configure = quiet}
      )
  );
  root->addChild(std::move(header));

  auto scroll =
      ui::scrollView({.out = &m_scroll, .contentScale = s, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F});
  scroll->clearFill();
  scroll->clearBorder();
  scroll->content()->setAlign(FlexAlign::Stretch);
  scroll->content()->setGap(12 * s);
  scroll->content()->addChild(ui::label({.out = &m_status, .fontSize = 14 * s, .maxLines = 4}));
  scroll->content()->addChild(ui::label({.out = &m_actionText, .fontSize = 14 * s, .maxLines = 0}));
  auto answer = std::make_unique<MarkdownView>();
  m_answer = answer.get();
  scroll->content()->addChild(std::move(answer));
  root->addChild(std::move(scroll));

  auto actions = ui::row({.out = &m_actions, .align = FlexAlign::Center, .gap = 10 * s});
  actions->addChild(
      ui::button(
          {.out = &m_retry,
           .text = i18n::tr("assistant.retry"),
           .glyph = "refresh",
           .fontSize = 12 * s,
           .glyphSize = 16 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .onClick =
               [this] {
                 m_copied = false;
                 m_restoreQuestionPending = true;
                 if (m_draft == m_ai.question()) {
                   m_draft.clear();
                   m_input->setValue("");
                 }
                 m_ai.retryConversation();
               },
           .configure = quiet}
      )
  );
  actions->addChild(
      ui::button(
          {.out = &m_settings,
           .text = i18n::tr("assistant.settings"),
           .fontSize = 12 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .onClick =
               [] {
                 DeferredCall::callLater([] {
                   PanelManager::instance().closePanelById("assistant");
                   PanelManager::instance().openSettingsWindow("launcher");
                 });
               },
           .configure = quiet}
      )
  );
  actions->addChild(ui::spacer());
  actions->addChild(
      ui::button(
          {.out = &m_copy,
           .glyph = "copy",
           .glyphSize = 16 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.copy-shortcut"),
           .onClick = [this] { copyAnswer(); },
           .configure = quiet}
      )
  );
  actions->addChild(
      ui::button(
          {.out = &m_speak,
           .glyph = "volume",
           .glyphSize = 16 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.voice.read-aloud"),
           .onClick = [this] { m_voice.speak(m_config->config().shell.launcher.ai.voice, std::string(answerText())); },
           .configure = quiet}
      )
  );
  root->addChild(std::move(actions));

  auto composer = ui::row({.out = &m_composer, .align = FlexAlign::Center, .gap = 8 * s});
  composer->addChild(
      ui::input(
          {.out = &m_input,
           .value = m_draft,
           .placeholder = i18n::tr("assistant.placeholder"),
           .fontSize = 15 * s,
           .controlHeight = 44 * s,
           .horizontalPadding = 12 * s,
           .lineEditing = true,
           .frameVisible = false,
           .flexGrow = 1.0F,
           .onChange =
               [this](const std::string& value) {
                 m_draft = value;
                 if (!StringUtils::trim(value).empty()) {
                   m_handoffPending = false;
                   m_handoffTimer.stop();
                 }
                 sync();
               },
           .onSubmit = [this](const std::string&) { submit(); }}
      )
  );
  composer->addChild(
      ui::button(
          {.out = &m_microphone,
           .glyph = "microphone",
           .glyphSize = 18 * s,
           .controlHeight = 36 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.voice.dictate"),
           .onClick = [this] { toggleDictation(); },
           .configure = quiet}
      )
  );
  composer->addChild(
      ui::button(
          {.out = &m_send,
           .glyph = "arrow-up",
           .glyphSize = 20 * s,
           .controlHeight = 36 * s,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("assistant.send"),
           .onClick = [this] { submit(); },
           .configure = quiet}
      )
  );
  root->addChild(std::move(composer));

  auto media = ui::row({.out = &m_media, .align = FlexAlign::Center, .gap = 12 * s, .visible = false});
  media->addChild(
      ui::image({.out = &m_art, .fit = ImageFit::Cover, .radius = 7 * s, .width = 34 * s, .height = 34 * s})
  );
  media->addChild(ui::label({.out = &m_track, .fontSize = 12 * s, .maxLines = 2, .flexGrow = 1.0F}));
  auto wave = std::make_unique<AudioVisualizer>();
  m_wave = wave.get();
  wave->setSize(26 * s, 22 * s);
  wave->setGradient(colorSpecFromRole(ColorRole::OnSurface), colorSpecFromRole(ColorRole::OnSurfaceVariant));
  media->addChild(std::move(wave));
  media->addChild(
      ui::button(
          {.out = &m_play,
           .glyph = "player-pause",
           .glyphSize = 18 * s,
           .controlHeight = 32 * s,
           .variant = ButtonVariant::Ghost,
           .onClick =
               [this] {
                 if (m_mpris)
                   m_mpris->playPauseActive();
               },
           .configure = quiet}
      )
  );
  root->addChild(std::move(media));
  setRoot(std::move(root));
  updateOrb();
}

void AssistantPanel::onOpen(std::string_view context) {
  m_open = true;
  if (context == "/dictate") {
    toggleDictation();
  } else if (context == "/hold-start") {
    beginHeldDictation();
  } else if (!context.empty()) {
    stop();
    m_dictated = false;
    m_draft = std::string(context);
    m_input->setValue(m_draft);
  }
  // A shortcut may update the open panel after Copy or a media button took focus.
  if (root() && root()->parent())
    PanelManager::instance().focusArea(m_input->inputArea());
  sync();
}

void AssistantPanel::onClose() {
  m_open = false;
  m_dictationHeld = false;
  m_ducking.setActive(false);
  m_handoffTimer.stop();
  m_handoffPending = false;
  if (m_localActions.pending()) {
    m_actionError = i18n::tr("assistant.actions.cancelled");
    if (m_draft.empty())
      m_draft = m_actionQuestion;
  }
  m_localActions.cancel();
  m_voiceEnergy = 0;
  m_replyPending = false;
  m_voice.cancel();
  m_ai.stopConversation();
  if (m_spectrumListener && m_spectrum)
    m_spectrum->removeChangeListener(m_spectrumListener);
  m_spectrumListener = 0;
  m_header = nullptr;
  m_activity = nullptr;
  m_activityLabel = nullptr;
  m_close = nullptr;
  m_orbExtent = 0;
  m_orbTargetExtent = 0;
  m_orb = nullptr;
  m_eyeTimer.stop();
  m_ambientTimer.stop();
  m_scroll = nullptr;
  m_status = nullptr;
  m_actionText = nullptr;
  m_answer = nullptr;
  m_actions = nullptr;
  m_copy = nullptr;
  m_retry = nullptr;
  m_finishDictation = nullptr;
  m_settings = nullptr;
  m_composer = nullptr;
  m_input = nullptr;
  m_send = nullptr;
  m_microphone = nullptr;
  m_speak = nullptr;
  m_media = nullptr;
  m_art = nullptr;
  m_track = nullptr;
  m_play = nullptr;
  m_wave = nullptr;
  m_renderedAnswer.clear();
  m_artPath.clear();
  m_pendingArt.clear();
  clearReleasedRoot();
}

void AssistantPanel::sync() {
  const auto voice = m_voice.state();
  m_ducking.setActive(
      m_open
      && m_config->config().shell.launcher.ai.voice.duckMedia
      && (voice == AssistantVoice::State::Recording
          || voice == AssistantVoice::State::PreparingSpeech
          || voice == AssistantVoice::State::Speaking)
  );
  if (!m_open)
    return;
  if (m_replyPending && !m_ai.streaming()) {
    m_replyPending = false;
    if (m_ai.error().empty() && !m_ai.interrupted() && !m_ai.answer().empty())
      m_voice.speak(m_config->config().shell.launcher.ai.voice, std::string(m_ai.answer()));
  }
  scheduleHandoff();
  PanelManager::instance().requestUpdateOnly();
  if (MotionService::instance().enabled())
    PanelManager::instance().requestFrameTick();
}

void AssistantPanel::submit() {
  if (!m_input || m_ai.streaming() || m_voice.busy() || m_localActions.pending())
    return;
  const auto question = m_input->value();
  m_handoffPending = false;
  m_handoffTimer.stop();
  if (const auto action = AssistantActions::parse(question)) {
    runAction(*action, question);
    return;
  }
  if (!m_ai.submitQuestion(question))
    return;
  m_showAction = false;
  m_actionReply.clear();
  m_actionError.clear();
  m_restoreQuestionPending = true;
  m_replyPending = m_dictated && m_config->config().shell.launcher.ai.voice.spokenReplies;
  m_dictated = false;
  m_voice.cancel();
  m_copied = false;
  m_draft.clear();
  m_input->setValue("");
  m_scroll->requestScrollToOffset(0);
  sync();
}

std::string_view AssistantPanel::answerText() const {
  return m_showAction ? std::string_view(m_actionReply) : m_ai.answer();
}

void AssistantPanel::runAction(const AssistantActions::Request& request, const std::string& question) {
  m_showAction = true;
  m_actionReply.clear();
  m_actionError.clear();
  m_actionQuestion = question;
  m_replyPending = false;
  m_voice.cancel();
  m_restoreQuestionPending = false;
  const bool speak = m_dictated && m_config->config().shell.launcher.ai.voice.spokenReplies;
  m_dictated = false;
  m_copied = false;
  m_draft.clear();
  m_input->setValue("");
  m_scroll->requestScrollToOffset(0);
  m_localActions.run(request, [this, question, speak](AssistantActions::Result result) {
    if (!m_open)
      return;
    if (result.success) {
      m_actionReply = std::move(result.text);
      m_handoffPending = result.handoff;
      if (speak)
        m_voice.speak(m_config->config().shell.launcher.ai.voice, m_actionReply, false);
    } else {
      m_actionError = std::move(result.text);
      if (m_draft.empty()) {
        m_draft = question;
        m_input->setValue(m_draft);
      }
    }
    sync();
  });
  sync();
}

void AssistantPanel::scheduleHandoff() {
  if (!m_handoffPending || !m_open || m_voice.busy() || m_handoffTimer.active())
    return;
  if (!m_voice.error().empty()) {
    m_handoffPending = false;
    return;
  }
  m_handoffTimer.start(std::chrono::milliseconds(1200), [this] {
    m_handoffTimer.stop();
    if (!m_open || !m_handoffPending || m_voice.busy() || !m_draft.empty())
      return;
    m_handoffPending = false;
    PanelManager::instance().closePanelById("assistant");
  });
}

void AssistantPanel::copyAnswer() {
  if (!answerText().empty() && m_clipboard)
    m_copied = m_clipboard->copyText(std::string(answerText()));
  sync();
}

void AssistantPanel::newConversation() {
  stop();
  m_dictated = false;
  m_ai.clearConversation();
  m_showAction = false;
  m_actionReply.clear();
  m_actionError.clear();
  m_draft.clear();
  m_copied = false;
  m_restoreQuestionPending = false;
  if (m_input) {
    m_input->setValue("");
    PanelManager::instance().focusArea(m_input->inputArea());
  }
  sync();
}

bool AssistantPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit)
    return false;
  if (sym == XKB_KEY_Escape && modifiers == 0) {
    stop();
    return false; // Let the panel manager dismiss the card as usual.
  }
  if (modifiers == (KeyMod::Ctrl | KeyMod::Shift) && (sym == XKB_KEY_c || sym == XKB_KEY_C)) {
    copyAnswer();
    return true;
  }
  if (modifiers == KeyMod::Ctrl && (sym == XKB_KEY_n || sym == XKB_KEY_N)) {
    newConversation();
    return true;
  }
  if (modifiers == KeyMod::Ctrl && (sym == XKB_KEY_d || sym == XKB_KEY_D)) {
    toggleDictation();
    return true;
  }
  if (modifiers == KeyMod::Ctrl
      && sym == XKB_KEY_period
      && (m_ai.streaming() || m_voice.busy() || m_localActions.pending())) {
    stop();
    return true;
  }
  return false;
}

void AssistantPanel::stop() {
  m_dictationHeld = false;
  m_handoffTimer.stop();
  m_handoffPending = false;
  if (m_localActions.pending()) {
    m_actionError = i18n::tr("assistant.actions.cancelled");
    if (m_draft.empty())
      m_draft = m_actionQuestion;
  }
  m_localActions.cancel();
  m_replyPending = false;
  m_voice.cancel();
  m_ai.stopConversation();
  if (m_input && m_open) {
    m_input->setEnabled(true);
    m_input->setValue(m_draft);
    if (root() && root()->parent())
      PanelManager::instance().focusArea(m_input->inputArea());
  }
}

void AssistantPanel::toggleDictation() {
  m_dictationHeld = false;
  if (m_voice.state() == AssistantVoice::State::Recording) {
    m_voice.finishRecording();
    return;
  }
  stop();
  m_voice.dictate(m_config->config().shell.launcher.ai.voice);
}

void AssistantPanel::beginHeldDictation() {
  // Independently launched press/release IPC clients can arrive in reverse order on a quick tap.
  if (m_dictationHeld || std::chrono::steady_clock::now() - m_holdReleased < std::chrono::milliseconds(180))
    return;
  if (m_voice.state() != AssistantVoice::State::Recording) {
    stop();
    m_voice.dictate(m_config->config().shell.launcher.ai.voice);
  }
  m_dictationHeld = m_voice.state() == AssistantVoice::State::Recording;
  m_holdStarted = std::chrono::steady_clock::now();
}

void AssistantPanel::finishHeldDictation() {
  m_holdReleased = std::chrono::steady_clock::now();
  if (!m_open || !m_dictationHeld)
    return;
  m_dictationHeld = false;
  if (std::chrono::steady_clock::now() - m_holdStarted < std::chrono::milliseconds(180))
    m_voice.cancel();
  else
    m_voice.finishRecording();
  sync();
}

void AssistantPanel::cancelVoice() {
  stop();
  sync();
}

std::string AssistantPanel::voiceStatus() const {
  constexpr std::array names{"idle", "recording", "transcribing", "preparing", "speaking"};
  return nlohmann::json{
             {"voice", names[static_cast<std::size_t>(m_voice.state())]},
             {"held", m_dictationHeld},
             {"error", m_voice.error()},
             {"streaming", m_ai.streaming()},
             {"has_answer", !answerText().empty()},
             {"has_draft", !m_draft.empty()}
         }.dump()
      + "\n";
}

void AssistantPanel::doUpdate(Renderer& renderer) {
  if (!m_open || !m_header)
    return;
  const bool busy = m_ai.streaming() || m_localActions.pending();
  const bool voiceBusy = m_voice.busy();
  const bool listening = m_voice.state() == AssistantVoice::State::Recording;
  const bool voiceFailed = !m_voice.error().empty();
  const bool failed = m_showAction ? !m_actionError.empty() : !m_ai.error().empty();
  const bool interrupted = !m_showAction && m_ai.interrupted();
  const bool hasAnswer = !answerText().empty();
  const char* title = m_localActions.pending() ? "assistant.actions.working"
      : busy                                   ? "assistant.thinking"
      : interrupted                            ? "assistant.stopped"
                                               : nullptr;
  if (m_ai.streaming() && m_ai.generationStage() == AiProvider::GenerationStage::LoadingModel)
    title = "assistant.loading-model";
  else if (m_ai.streaming() && m_ai.generationStage() == AiProvider::GenerationStage::Waiting)
    title = "assistant.waiting-strata";
  updateOrb();
  switch (m_voice.state()) {
  case AssistantVoice::State::Recording:
    title = "assistant.voice.listening";
    break;
  case AssistantVoice::State::Transcribing:
    title = "assistant.voice.transcribing";
    break;
  case AssistantVoice::State::PreparingSpeech:
    title = "assistant.voice.preparing";
    break;
  case AssistantVoice::State::Speaking:
    title = "assistant.voice.speaking";
    break;
  case AssistantVoice::State::Idle:
    break;
  }
  m_activity->setVisible(title != nullptr);
  m_activityLabel->setText(title ? i18n::tr(title) : std::string{});
  float orbExtent = compact() ? 72.0F : 88.0F;
  if (listening)
    orbExtent = compact() ? 92.0F : 112.0F;
  else if (voiceBusy)
    orbExtent = compact() ? 80.0F : 96.0F;
  else if (busy || failed || voiceFailed)
    orbExtent = compact() ? 64.0F : 80.0F;
  else if (hasAnswer)
    orbExtent = compact() ? 56.0F : 64.0F;
  m_orbTargetExtent = orbExtent;
  if (m_orbExtent == 0 || !MotionService::instance().enabled())
    m_orbExtent = m_orbTargetExtent;
  const bool showError = !busy && !voiceBusy && (failed || voiceFailed);
  m_scroll->setVisible(hasAnswer || showError);
  m_status->setVisible(showError);
  m_status->setText(
      voiceFailed  ? m_voice.error()
          : failed ? (m_showAction ? m_actionError : std::string(m_ai.error()))
                   : std::string{}
  );
  m_actionText->setVisible(m_showAction && hasAnswer);
  m_actionText->setText(m_showAction ? m_actionReply : std::string{});
  if (!m_showAction && m_renderedAnswer != answerText()) {
    m_renderedAnswer = answerText();
    m_answer->setMarkdown(m_renderedAnswer, contentScale());
  }
  m_answer->setVisible(!m_showAction && hasAnswer);
  m_actions->setVisible(
      !busy && !voiceBusy && (voiceFailed || (!m_showAction && (hasAnswer || failed || interrupted)))
  );
  m_copy->setVisible(!m_showAction && hasAnswer && !busy && !voiceBusy);
  m_copy->setGlyph(m_copied ? "check" : "copy");
  m_copy->setTooltip(i18n::tr(m_copied ? "assistant.copied" : "assistant.copy-shortcut"));
  m_retry->setVisible(!m_showAction && !busy && !voiceBusy && (failed || interrupted));
  m_finishDictation->setVisible(listening);
  m_settings->setVisible(showError && (!m_showAction || voiceFailed));
  const bool voiceEnabled = m_config->config().shell.launcher.ai.voice.enabled;
  m_speak->setVisible(!m_showAction && voiceEnabled && hasAnswer && !busy && !voiceBusy);
  if (!m_showAction && !busy && !voiceBusy && (failed || interrupted) && m_restoreQuestionPending) {
    m_restoreQuestionPending = false;
    if (m_draft.empty()) {
      m_draft = m_ai.question();
      m_input->setValue(m_draft);
    }
  }
  const bool hasDraft = !StringUtils::trim(m_draft).empty();
  const bool restoreFocus = !m_composer->visible() && !voiceBusy;
  m_composer->setVisible(!voiceBusy);
  m_microphone->setVisible(voiceEnabled && !busy && !voiceBusy && !hasDraft);
  m_microphone->setEnabled(!busy && !voiceBusy);
  m_input->setEnabled(!voiceBusy);
  if (restoreFocus)
    PanelManager::instance().focusArea(m_input->inputArea());
  m_input->setPlaceholder(i18n::tr(m_ai.question().empty() ? "assistant.placeholder" : "assistant.follow-up"));
  m_send->setVisible(!busy && !voiceBusy && hasDraft);
  m_send->setEnabled(!busy && !voiceBusy && hasDraft);
  // The input stays available to prepare the next question while the answer streams.
  updateMedia(renderer);
}

void AssistantPanel::doLayout(Renderer& renderer, float width, float height) {
  if (!m_header)
    return;
  const float s = contentScale();
  const float gap = 12 * s;
  m_status->setMaxWidth(std::max(1.0F, width - 8 * s));
  m_actionText->setMaxWidth(std::max(1.0F, width - m_scroll->scrollbarGutter()));
  const bool body = m_scroll->visible();
  const bool actions = m_actions->visible();
  const bool composer = m_composer->visible();
  const bool media = m_media->visible();
  LayoutConstraints constraints;
  constraints.setExactWidth(std::max(1.0F, width - m_scroll->scrollbarGutter()));
  const float wantedBody = body
      ? std::clamp(m_scroll->content()->measure(renderer, constraints).height, 24 * s, (compact() ? 220 : 300) * s)
      : 0;
  const float headerHeight = m_orbExtent + (m_activity->visible() ? 36.0F : 0.0F);
  const float fixed = (headerHeight + (composer ? 44.0F : 0.0F) + (actions ? 32.0F : 0.0F) + (media ? 46.0F : 0.0F)) * s
      + gap
          * (static_cast<float>(composer)
             + static_cast<float>(body)
             + static_cast<float>(actions)
             + static_cast<float>(media));
  m_fittedHeight = fixed + wantedBody + 2 * Style::panelPadding * s;
  m_bodyHeight = body ? std::max(1.0F, std::min(wantedBody, height - fixed)) : 0;
  float y = 0;
  auto place = [&](Node* node, float h) {
    node->setPosition(0, y);
    node->setSize(width, h);
    node->layout(renderer);
    y += h + gap;
  };
  const float orbSize = m_orbExtent * s;
  m_orb->setSize(orbSize, orbSize);
  m_orb->setPosition((width - orbSize) / 2, 0);
  m_close->setSize(32 * s, 32 * s);
  m_close->setPosition(width - 32 * s, 0);
  m_activity->setPosition(0, orbSize + 4 * s);
  m_activity->setSize(width, 32 * s);
  place(m_header, headerHeight * s);
  if (body)
    place(m_scroll, m_bodyHeight);
  if (actions)
    place(m_actions, 32 * s);
  if (composer)
    place(m_composer, 44 * s);
  if (media) {
    m_track->setMaxWidth(std::max(1.0F, width - 130 * s));
    place(m_media, 46 * s);
  }
}

void AssistantPanel::updateMedia(Renderer& renderer) {
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const bool visible = player && (player->playbackStatus == "Playing" || player->playbackStatus == "Paused");
  m_media->setVisible(visible);
  m_playing = visible && player->playbackStatus == "Playing";
  syncSpectrum();
  if (!visible)
    return;
  m_track->setText(player->title.empty() ? player->identity : player->title);
  m_play->setGlyph(m_playing ? "player-pause" : "player-play");
  m_play->setTooltip(i18n::tr(m_playing ? "assistant.pause-media" : "assistant.play-media"));
  m_play->setEnabled(player->canControl && (m_playing ? player->canPause : player->canPlay));
  m_wave->setVisible(m_playing);
  const auto path =
      mpris::resolveArtworkSource(m_http, m_pendingArt, mpris::effectiveArtUrl(*player), [this] { sync(); }, m_alive);
  if (path != m_artPath || (!path.empty() && !m_art->hasImage())) {
    m_artPath = path;
    if (path.empty() || !m_art->setSourceFile(renderer, path, static_cast<int>(68 * contentScale()), true, true))
      m_art->clear(renderer);
  }
  m_art->setVisible(m_art->hasImage());
}

void AssistantPanel::syncSpectrum() {
  const bool listen = m_open && m_playing && m_spectrum && m_wave;
  if (listen && !m_spectrumListener) {
    m_spectrumListener = m_spectrum->addChangeListener(5, [] { PanelManager::instance().requestFrameTick(); });
    PanelManager::instance().requestFrameTick();
  } else if (!listen && m_spectrumListener) {
    m_spectrum->removeChangeListener(m_spectrumListener);
    m_spectrumListener = 0;
  }
}

void AssistantPanel::scheduleEyeAnimation() {
  if (!m_open || !m_orb || m_eyeTimer.active() || !MotionService::instance().enabled())
    return;
  m_eyeTimer.start(std::chrono::milliseconds(2800 + (m_eyeAnimations++ * 917) % 2400), [this] {
    m_eyeTimer.stop();
    if (!m_open || !m_orb || !MotionService::instance().enabled())
      return;
    m_orb->animateEyes();
    PanelManager::instance().requestFrameTick();
    scheduleEyeAnimation();
  });
}

void AssistantPanel::syncAmbientAnimation() {
  if (!m_open || !MotionService::instance().enabled()) {
    m_ambientTimer.stop();
    return;
  }
  if (!m_ambientTimer.active())
    m_ambientTimer.startRepeating(std::chrono::milliseconds(33), [this] {
      if (!m_open || !MotionService::instance().enabled()) {
        m_ambientTimer.stop();
        return;
      }
      PanelManager::instance().requestFrameTick();
    });
}

void AssistantPanel::updateOrb() {
  if (!m_orb)
    return;
  using State = AssistantOrb::State;
  State state = State::Idle;
  if (m_voice.state() == AssistantVoice::State::Recording)
    state = State::Listening;
  else if (m_voice.state() == AssistantVoice::State::Speaking)
    state = State::Speaking;
  else if (m_ai.streaming() || m_voice.busy() || m_localActions.pending())
    state = State::Thinking;
  else if (!m_voice.error().empty() || (m_showAction ? !m_actionError.empty() : !m_ai.error().empty()))
    state = State::Error;
  else if (!answerText().empty() && (m_showAction || !m_ai.interrupted()))
    state = State::Complete;
  if (m_orb->setState(state))
    m_eyeTimer.stop();
  if (m_orb->tick(0, m_voiceEnergy, MotionService::instance().enabled()))
    PanelManager::instance().requestFrameTick();
  scheduleEyeAnimation();
  syncAmbientAnimation();
}

void AssistantPanel::onFrameTick(float deltaMs) {
  if (!m_open)
    return;
  bool again = false;
  if (m_orbExtent != m_orbTargetExtent) {
    const float dt = std::clamp(deltaMs, 0.0F, 100.0F) * MotionService::instance().speed();
    if (MotionService::instance().enabled())
      m_orbExtent += (m_orbTargetExtent - m_orbExtent) * (1.0F - std::exp(-dt / 90.0F));
    if (!MotionService::instance().enabled() || std::abs(m_orbTargetExtent - m_orbExtent) < 0.05F)
      m_orbExtent = m_orbTargetExtent;
    PanelManager::instance().requestLayout();
  }
  const bool active = m_ai.streaming() || m_voice.busy() || m_localActions.pending();
  if (MotionService::instance().enabled() && (active || m_voiceEnergy > 0.001F)) {
    const float dt = std::clamp(deltaMs, 0.0F, 100.0F);
    const float target = m_voice.audioLevel();
    const float response = target > m_voiceEnergy ? 45.0F : 180.0F;
    m_voiceEnergy += (target - m_voiceEnergy) * (1.0F - std::exp(-dt / response));
    if (m_voiceEnergy < 0.001F)
      m_voiceEnergy = 0;
  } else if (!MotionService::instance().enabled()) {
    m_voiceEnergy = 0;
    updateOrb();
  }
  if (m_orb)
    m_orb->tick(deltaMs, m_voiceEnergy, MotionService::instance().enabled());
  if (m_spectrumListener && m_wave) {
    m_wave->setValues(m_spectrum->values(m_spectrumListener));
    m_wave->setSmoothingTimeMs(MotionService::instance().enabled() ? 60.0F : 0.0F);
    m_wave->tick(deltaMs);
    again |= !m_wave->converged() || !m_spectrum->idle();
  }
  PanelManager::instance().requestRedraw();
  if (again)
    PanelManager::instance().requestFrameTick();
}
