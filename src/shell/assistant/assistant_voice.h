#pragma once

#include "config/config_types.h"
#include "core/process/process.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class AssistantAudioLevel;
// Local, on-demand processes. All completions return to the shell's main loop.
class AssistantVoice {
public:
  using Config = ShellConfig::LauncherConfig::AiConfig::VoiceConfig;
  enum class State { Idle, Recording, Transcribing, PreparingSpeech, Speaking };
  ~AssistantVoice();
  void dictate(const Config& config);
  void finishRecording();
  void speak(const Config& config, std::string text, bool markdown = true);
  void cancel();
  void setChanged(std::function<void()> callback) { m_changed = std::move(callback); }
  void setTranscript(std::function<void(std::string)> callback) { m_transcript = std::move(callback); }
  [[nodiscard]] State state() const { return m_state; }
  [[nodiscard]] bool busy() const { return m_state != State::Idle; }
  [[nodiscard]] const std::string& error() const { return m_error; }
  [[nodiscard]] float audioLevel() const;

private:
  struct Workspace {
    std::filesystem::path path;
    ~Workspace();
  };
  bool workspace();
  void
  run(std::vector<std::string> args, int seconds, std::function<void(process::RunResult)> done,
      process::OutputCallback output = {}, process::OutputCallback diagnostic = {});
  void fail(std::string key);
  void changed();
  void transcribe();

  Config m_config;
  State m_state = State::Idle;
  std::string m_error;
  std::function<void()> m_changed;
  std::function<void(std::string)> m_transcript;
  std::shared_ptr<Workspace> m_workspace;
  std::shared_ptr<AssistantAudioLevel> m_level;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  std::shared_ptr<void> m_alive = std::make_shared<int>(0);
  std::uint64_t m_generation = 0;
};
