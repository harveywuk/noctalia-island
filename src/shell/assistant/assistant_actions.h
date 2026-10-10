#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

class IpcService;
class MprisService;
class PipeWireService;
class UPowerService;

// Only a complete, explicitly submitted command can reach a shell control.
// Model output is never parsed or executed.
class AssistantActions {
public:
  enum class Kind {
    Timer,
    TimerStatus,
    PauseTimer,
    ResumeTimer,
    CancelTimer,
    PauseMedia,
    ResumeMedia,
    MediaStatus,
    NextMedia,
    Volume,
    Focus,
    FocusStatus,
    BatteryStatus,
    Invalid
  };
  struct Request {
    Kind kind = Kind::Invalid;
    int amount = 0;
    std::string mode;
    std::string errorKey;
  };
  struct Result {
    std::string text;
    bool success = false;
    bool handoff = false;
  };
  using Completion = std::function<void(Result)>;

  AssistantActions(IpcService* ipc, PipeWireService* audio, MprisService* media, UPowerService* power);
  [[nodiscard]] static std::optional<Request> parse(std::string_view text);
  void run(const Request& request, Completion completion);
  void cancel();
  [[nodiscard]] bool pending() const { return static_cast<bool>(m_completion); }

private:
  void finish(Result result);
  IpcService* m_ipc;
  PipeWireService* m_audio;
  MprisService* m_media;
  UPowerService* m_power;
  Timer m_poll;
  Completion m_completion;
};
