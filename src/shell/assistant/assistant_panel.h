#pragma once

#include "launcher/ai_provider.h"
#include "shell/assistant/assistant_actions.h"
#include "shell/assistant/assistant_ducking.h"
#include "shell/assistant/assistant_voice.h"
#include "shell/panel/panel.h"

#include <array>
#include <unordered_set>

class AudioVisualizer;
class AssistantOrb;
class Button;
class ClipboardService;
class ConfigService;
class Flex;
class Image;
class Input;
class Label;
class MarkdownView;
class MprisService;
class IpcService;
class PipeWireService;
class PipeWireSpectrum;
class UPowerService;
class ScrollView;

// Invoked by a compositor shortcut. Conversation and draft live only in memory.
class AssistantPanel final : public Panel {
public:
  AssistantPanel(
      ConfigService* config, ClipboardService* clipboard, HttpClient* http, MprisService* mpris,
      PipeWireSpectrum* spectrum, IpcService* ipc, PipeWireService* audio, UPowerService* power
  );
  ~AssistantPanel() override;
  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void finishHeldDictation();
  void cancelVoice();
  [[nodiscard]] std::string voiceStatus() const;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] float fittedHeight() const override;
  [[nodiscard]] float islandHeight(float availableHeight) const override;
  [[nodiscard]] std::optional<std::array<Color, 4>> islandAccentColors() const override;
  [[nodiscard]] bool retainsIslandArtwork() const override { return true; }
  [[nodiscard]] std::string islandArtworkSource() const override { return m_playing ? m_artPath : std::string{}; }
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit();
  void runAction(const AssistantActions::Request& request, const std::string& question);
  void scheduleHandoff();
  std::string_view answerText() const;
  void toggleDictation();
  void beginHeldDictation();
  void stop();
  void sync();
  void updateMedia(Renderer& renderer);
  void syncSpectrum();
  void updateOrb();
  void scheduleEyeAnimation();
  void syncAmbientAnimation();
  void copyAnswer();
  void newConversation();
  bool compact() const;

  AiProvider m_ai;
  AssistantVoice m_voice;
  AssistantActions m_localActions;
  AssistantDucking m_ducking;
  bool m_dictationHeld = false;
  std::chrono::steady_clock::time_point m_holdStarted;
  std::chrono::steady_clock::time_point m_holdReleased;
  Timer m_handoffTimer;
  std::string m_actionReply;
  std::string m_actionQuestion;
  std::string m_actionError;
  bool m_showAction = false;
  bool m_handoffPending = false;
  ConfigService* m_config;
  ClipboardService* m_clipboard;
  HttpClient* m_http;
  MprisService* m_mpris;
  PipeWireSpectrum* m_spectrum;
  Node* m_header = nullptr;
  Flex* m_activity = nullptr;
  Label* m_activityLabel = nullptr;
  Button* m_close = nullptr;
  AssistantOrb* m_orb = nullptr;
  Timer m_eyeTimer;
  Timer m_ambientTimer;
  unsigned m_eyeAnimations = 0;
  ScrollView* m_scroll = nullptr;
  Label* m_status = nullptr;
  Label* m_actionText = nullptr;
  MarkdownView* m_answer = nullptr;
  Flex* m_actions = nullptr;
  Button* m_copy = nullptr;
  Button* m_retry = nullptr;
  Button* m_finishDictation = nullptr;
  Button* m_settings = nullptr;
  Flex* m_composer = nullptr;
  Input* m_input = nullptr;
  Button* m_send = nullptr;
  Button* m_microphone = nullptr;
  Button* m_speak = nullptr;
  Flex* m_media = nullptr;
  Image* m_art = nullptr;
  Label* m_track = nullptr;
  Button* m_play = nullptr;
  AudioVisualizer* m_wave = nullptr;
  std::string m_draft;
  std::string m_renderedAnswer;
  std::string m_artPath;
  std::unordered_set<std::string> m_pendingArt;
  std::shared_ptr<void> m_alive = std::make_shared<int>(0);
  std::uint64_t m_spectrumListener = 0;
  float m_bodyHeight = 0;
  float m_fittedHeight = 160;
  float m_voiceEnergy = 0;
  float m_orbExtent = 0;
  float m_orbTargetExtent = 0;
  bool m_open = false;
  bool m_playing = false;
  bool m_copied = false;
  bool m_dictated = false;
  bool m_replyPending = false;
  bool m_restoreQuestionPending = false;
};
