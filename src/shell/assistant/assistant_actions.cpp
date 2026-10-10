#include "shell/assistant/assistant_actions.h"

#include "dbus/mpris/mpris_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "launcher/timer_provider.h"
#include "pipewire/pipewire_service.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_state_store.h"
#include "shell/island/island_timer.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

namespace {
  using Kind = AssistantActions::Kind;
  using Request = AssistantActions::Request;
  using Result = AssistantActions::Result;

  std::string normalized(std::string_view text) {
    std::istringstream input(StringUtils::toLower(StringUtils::trim(text)));
    std::string word, out;
    while (input >> word) {
      if (!out.empty())
        out += ' ';
      out += word;
    }
    // Dictation may use a typographic apostrophe in contractions.
    for (auto pos = out.find("’"); pos != std::string::npos; pos = out.find("’", pos + 1))
      out.replace(pos, std::string_view("’").size(), "'");
    while (!out.empty() && std::string_view(".!?").find(out.back()) != std::string_view::npos)
      out.pop_back();
    for (const auto prefix : {"please ", "can you ", "could you ", "would you ", "please "})
      if (out.starts_with(prefix))
        out.erase(0, std::char_traits<char>::length(prefix));
    if (out.ends_with(" please"))
      out.resize(out.size() - 7);
    return out;
  }

  std::optional<int> number(std::string text) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= 0)
      return value;
    std::replace(text.begin(), text.end(), '-', ' ');
    static const std::map<std::string, int> small{
        {"zero", 0},          {"a", 1},          {"an", 1},        {"one", 1},       {"two", 2},       {"three", 3},
        {"four", 4},          {"five", 5},       {"six", 6},       {"seven", 7},     {"eight", 8},     {"nine", 9},
        {"ten", 10},          {"eleven", 11},    {"twelve", 12},   {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15},
        {"sixteen", 16},      {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},   {"thirty", 30},
        {"forty", 40},        {"fifty", 50},     {"sixty", 60},    {"seventy", 70},  {"eighty", 80},   {"ninety", 90},
        {"one hundred", 100}, {"a hundred", 100}
    };
    if (const auto it = small.find(text); it != small.end())
      return it->second;
    const auto space = text.find(' ');
    if (space != std::string::npos) {
      const auto tens = small.find(text.substr(0, space)), ones = small.find(text.substr(space + 1));
      if (tens != small.end()
          && ones != small.end()
          && tens->second >= 20
          && tens->second < 100
          && tens->second % 10 == 0
          && ones->second > 0
          && ones->second < 10)
        return tens->second + ones->second;
    }
    return std::nullopt;
  }

  std::optional<int> duration(std::string text) {
    if (text == "half an hour")
      return 1800;
    // Consume the entire duration. Trailing labels and compound commands are not commands.
    static const std::regex part(
        R"(^([a-z -]+?|[0-9]+)\s*(hours?|hrs?|h|minutes?|mins?|m|seconds?|secs?|s)(?=[0-9]|\s|$)(?:\s+and\s+|\s*|$))"
    );
    int total = 0, previousUnit = 86401;
    std::smatch match;
    while (!text.empty()) {
      if (!std::regex_search(text, match, part))
        return std::nullopt;
      const auto amount = number(StringUtils::trim(match[1].str()));
      const auto unitText = match[2].str();
      const int unit = unitText[0] == 'h' ? 3600 : unitText[0] == 'm' ? 60 : 1;
      if (!amount || *amount <= 0 || unit >= previousUnit || *amount > (86400 - total) / unit)
        return std::nullopt;
      total += *amount * unit;
      previousUnit = unit;
      text = match.suffix().str();
    }
    return total > 0 ? std::optional(total) : std::nullopt;
  }

  Result error(const char* key) { return {.text = i18n::tr(key)}; }
  nlohmann::json timerValue(const char* key) {
    const auto value = scripting::PluginStateStore::instance().get("noctalia/timer", key);
    return value ? nlohmann::json::parse(*value, nullptr, false) : nlohmann::json{};
  }
  std::optional<island::Countdown> currentTimer() {
    return island::timerSnapshot(
        timerValue("timer.state"), timerValue("timer.remaining"), timerValue("timer.duration")
    );
  }
} // namespace

std::optional<AssistantActions::Request> AssistantActions::parse(std::string_view text) {
  if (text.size() > 512)
    return std::nullopt;
  const auto command = normalized(text);
  std::smatch match;
  static const std::map<std::string, Kind> queries{
      {"how long is left", Kind::TimerStatus},
      {"how much time is left", Kind::TimerStatus},
      {"how long is left on the timer", Kind::TimerStatus},
      {"how much time is left on my timer", Kind::TimerStatus},
      {"how much time is left on the timer", Kind::TimerStatus},
      {"timer status", Kind::TimerStatus},
      {"what's playing", Kind::MediaStatus},
      {"what is playing", Kind::MediaStatus},
      {"what's playing now", Kind::MediaStatus},
      {"what song is playing", Kind::MediaStatus},
      {"what track is playing", Kind::MediaStatus},
      {"what's my battery level", Kind::BatteryStatus},
      {"what is my battery level", Kind::BatteryStatus},
      {"how much battery is left", Kind::BatteryStatus},
      {"battery level", Kind::BatteryStatus},
      {"battery status", Kind::BatteryStatus},
      {"which focus is active", Kind::FocusStatus},
      {"what focus is active", Kind::FocusStatus},
      {"which focus mode is active", Kind::FocusStatus},
      {"focus status", Kind::FocusStatus}
  };
  if (const auto it = queries.find(command); it != queries.end())
    return Request{.kind = it->second};
  static const std::regex timerControl(R"(^(pause|resume|cancel|stop) (?:the |my )?timer$)");
  if (std::regex_match(command, match, timerControl))
    return Request{
        .kind = match[1] == "pause" ? Kind::PauseTimer
            : match[1] == "resume"  ? Kind::ResumeTimer
                                    : Kind::CancelTimer
    };
  static const std::regex nextTrack(
      R"(^(?:skip (?:this |the |current )?(?:track|song)|(?:play )?(?:the )?next (?:track|song))$)"
  );
  if (std::regex_match(command, nextTrack))
    return Request{.kind = Kind::NextMedia};
  static const std::regex timer(
      R"(^(?:set|start) (?:a |an )?timer(?: for)?(?: (.*))?$|^timer (.*)$|^(?:set|start) (?:a |an )?(.+) timer$)"
  );
  if (std::regex_match(command, match, timer)) {
    const auto value = duration(match[1].matched ? match[1].str() : match[2].matched ? match[2].str() : match[3].str());
    if (!value)
      return Request{.errorKey = "assistant.actions.timer-duration"};
    return Request{.kind = Kind::Timer, .amount = *value};
  }
  static const std::regex media(R"(^(pause|resume|play) (?:the |my )?(?:music|media|playback)$)");
  if (std::regex_match(command, match, media))
    return Request{.kind = match[1] == "pause" ? Kind::PauseMedia : Kind::ResumeMedia};
  static const std::regex volume(
      R"(^(?:set|change) (?:the |my )?(?:volume|speaker volume|system volume)(?: to)?(?: (.*))?$)"
  );
  if (std::regex_match(command, match, volume)) {
    auto value = match[1].str();
    if (value.ends_with('%'))
      value.pop_back();
    else if (value.ends_with(" percent"))
      value.resize(value.size() - 8);
    const auto amount = number(StringUtils::trim(value));
    if (!amount || *amount > 100)
      return Request{.errorKey = "assistant.actions.volume-range"};
    return Request{.kind = Kind::Volume, .amount = *amount};
  }
  static const std::regex focus(
      R"(^(enable|disable|turn on|turn off) (?:the )?(?:(work|gaming|sleep) )?focus(?: mode)?$|^(?:set|change) focus(?: mode)? to (work|gaming|sleep|off|auto)$)"
  );
  if (std::regex_match(command, match, focus)) {
    std::string mode = match[3].matched                     ? match[3].str()
        : (match[1] == "disable" || match[1] == "turn off") ? "off"
        : match[2].matched                                  ? match[2].str()
                                                            : "work";
    return Request{.kind = Kind::Focus, .mode = std::move(mode)};
  }
  // Reject recognizable commands with extra clauses instead of executing only their first part.
  if (command.starts_with("pause the music ")
      || command.starts_with("resume the music ")
      || command.starts_with("play the music ")
      || command.starts_with("enable focus ")
      || command.starts_with("disable focus ")
      || command.starts_with("set focus "))
    return Request{.errorKey = "assistant.actions.one-command"};
  static const std::regex extraClause(
      R"(^(?:(?:pause|resume|cancel|stop) (?:the |my )?timer|skip (?:this |the |current )?(?:track|song)|(?:play )?(?:the )?next (?:track|song)) .+$)"
  );
  if (std::regex_match(command, extraClause))
    return Request{.errorKey = "assistant.actions.one-command"};
  return std::nullopt;
}

AssistantActions::AssistantActions(IpcService* ipc, PipeWireService* audio, MprisService* media, UPowerService* power)
    : m_ipc(ipc), m_audio(audio), m_media(media), m_power(power) {}

void AssistantActions::cancel() {
  m_poll.stop();
  m_completion = {};
}

void AssistantActions::finish(Result result) {
  m_poll.stop();
  auto completion = std::move(m_completion);
  m_completion = {};
  if (completion)
    completion(std::move(result));
}

void AssistantActions::run(const Request& request, Completion completion) {
  cancel();
  m_completion = std::move(completion);
  if (request.kind == Kind::Invalid) {
    finish(error(request.errorKey.c_str()));
    return;
  }
  std::function<bool()> confirmed;
  Result result{.success = true};
  const char* failure = "assistant.actions.failed";
  switch (request.kind) {
  case Kind::TimerStatus:
  case Kind::PauseTimer:
  case Kind::ResumeTimer:
  case Kind::CancelTimer: {
    if (!scripting::PluginRegistry::instance().hasEntry("noctalia/timer:timer")) {
      finish(error("assistant.actions.timer-unavailable"));
      return;
    }
    const auto timer = currentTimer();
    if (!timer) {
      finish(error("assistant.actions.timer-state-unavailable"));
      return;
    }
    if (!timer->active) {
      finish({.text = i18n::tr("assistant.actions.timer-none"), .success = true});
      return;
    }
    const bool ended = timer->finished || timer->remaining == 0;
    if (request.kind == Kind::TimerStatus) {
      result.text = ended
          ? i18n::tr("assistant.actions.timer-finished")
          : i18n::tr(
                timer->running ? "assistant.actions.timer-remaining" : "assistant.actions.timer-paused-remaining",
                "duration", TimerProvider::formatDuration(std::chrono::seconds(timer->remaining))
            );
      finish(std::move(result));
      return;
    }
    const bool reset = request.kind == Kind::CancelTimer;
    const bool pause = request.kind == Kind::PauseTimer;
    if (ended && !reset) {
      finish({.text = i18n::tr("assistant.actions.timer-finished"), .success = true});
      return;
    }
    // Repeated pause/resume requests are harmless; only send a command when state must change.
    if (reset || timer->running == pause)
      scripting::PluginStateStore::instance().set(
          "noctalia/timer", "timer.cmd",
          nlohmann::json(
              reset       ? "RESET"
                  : pause ? "PAUSE"
                          : "START"
          )
              .dump()
      );
    confirmed = [reset, pause, duration = timer->duration] {
      const auto now = currentTimer();
      return now
          && (reset ? !now->active && now->remaining == 0 && now->duration == 0
                    : now->active
                      && !now->finished
                      && now->remaining > 0
                      && now->duration == duration
                      && now->running != pause);
    };
    result.text = i18n::tr(
        reset       ? "assistant.actions.timer-cancelled"
            : pause ? "assistant.actions.timer-paused"
                    : "assistant.actions.timer-resumed"
    );
    result.handoff = !reset;
    break;
  }
  case Kind::Timer: {
    if (!scripting::PluginRegistry::instance().hasEntry("noctalia/timer:timer")
        || timerValue("timer.state").is_null()) {
      finish(error("assistant.actions.timer-unavailable"));
      return;
    }
    if (timerValue("timer.state") != "IDLE") {
      finish(error("assistant.actions.timer-active"));
      return;
    }
    auto& store = scripting::PluginStateStore::instance();
    store.set("noctalia/timer", "timer.remaining", std::to_string(request.amount));
    store.set("noctalia/timer", "timer.cmd", "\"START\"");
    confirmed = [seconds = request.amount] {
      return timerValue("timer.state") == "RUNNING" && timerValue("timer.duration") == seconds;
    };
    result.text = i18n::tr(
        "assistant.actions.timer-started", "duration",
        TimerProvider::formatDuration(std::chrono::seconds(request.amount))
    );
    result.handoff = true;
    break;
  }
  case Kind::Volume: {
    if (!m_ipc || !m_audio || !m_audio->defaultSink()) {
      finish(error("assistant.actions.no-output"));
      return;
    }
    const auto id = m_audio->defaultSink()->id;
    if (m_ipc->execute("volume-set " + std::to_string(request.amount) + "%") != "ok\n") {
      finish(error("assistant.actions.failed"));
      return;
    }
    confirmed = [this, id, percent = request.amount] {
      const auto* sink = m_audio->defaultSink();
      return sink && sink->id == id && std::abs(sink->volume - static_cast<float>(percent) / 100.0F) < .015F;
    };
    result.text = i18n::tr("assistant.actions.volume-set", "percent", std::to_string(request.amount));
    break;
  }
  case Kind::MediaStatus:
  case Kind::NextMedia:
  case Kind::PauseMedia:
  case Kind::ResumeMedia: {
    const auto player = m_media ? m_media->activePlayer() : std::nullopt;
    if (!player) {
      finish(error("assistant.actions.no-media"));
      return;
    }
    if (request.kind == Kind::MediaStatus) {
      if (player->playbackStatus == "Stopped")
        result.text = i18n::tr("assistant.actions.media-stopped");
      else if (player->title.empty())
        result.text = i18n::tr("assistant.actions.media-no-title");
      else {
        const auto artists = joinedArtists(player->artists);
        const auto track = artists.empty()
            ? player->title
            : i18n::tr("assistant.actions.media-track", "title", player->title, "artist", artists);
        result.text = i18n::tr(
            player->playbackStatus == "Playing" ? "assistant.actions.media-playing"
                                                : "assistant.actions.media-current-paused",
            "track", track
        );
      }
      finish(std::move(result));
      return;
    }
    if (request.kind == Kind::NextMedia) {
      if (!m_media->next(player->busName)) {
        finish(error("assistant.actions.media-next-unavailable"));
        return;
      }
      confirmed = [this, bus = player->busName, track = logicalTrackSignature(*player)] {
        const auto& players = m_media->players();
        const auto it = players.find(bus);
        return it != players.end() && logicalTrackSignature(it->second) != track;
      };
      result.text = i18n::tr("assistant.actions.media-skipped");
      break;
    }
    const bool pause = request.kind == Kind::PauseMedia;
    const std::string status = pause ? "Paused" : "Playing";
    if (player->playbackStatus != status
        && !(pause ? m_media->pause(player->busName) : m_media->play(player->busName))) {
      finish(error("assistant.actions.media-unavailable"));
      return;
    }
    confirmed = [this, bus = player->busName, status] {
      const auto& players = m_media->players();
      const auto it = players.find(bus);
      return it != players.end() && it->second.playbackStatus == status;
    };
    result.text = i18n::tr(pause ? "assistant.actions.media-paused" : "assistant.actions.media-resumed");
    break;
  }
  case Kind::BatteryStatus: {
    if (!m_power || !m_power->state().isPresent) {
      finish({.text = i18n::tr("assistant.actions.battery-none"), .success = true});
      return;
    }
    const auto& battery = m_power->state();
    if (!std::isfinite(battery.percentage) || battery.percentage < 0 || battery.percentage > 100) {
      finish(error("assistant.actions.battery-unavailable"));
      return;
    }
    result.text = i18n::tr(
        battery.state == BatteryState::Charging           ? "assistant.actions.battery-charging"
            : battery.state == BatteryState::FullyCharged ? "assistant.actions.battery-full"
                                                          : "assistant.actions.battery-level",
        "percent", std::to_string(std::lround(battery.percentage))
    );
    finish(std::move(result));
    return;
  }
  case Kind::FocusStatus: {
    const auto status =
        m_ipc ? nlohmann::json::parse(m_ipc->execute("focus-status"), nullptr, false) : nlohmann::json{};
    if (!status.is_object()
        || !status.contains("mode")
        || !status["mode"].is_string()
        || !status.contains("automatic")
        || !status["automatic"].is_boolean()) {
      finish(error("assistant.actions.focus-unavailable"));
      return;
    }
    const auto mode = status["mode"].get<std::string>();
    const char* key = mode == "off" ? "assistant.actions.focus-off"
        : mode == "work"            ? "assistant.actions.focus-work"
        : mode == "gaming"          ? "assistant.actions.focus-gaming"
        : mode == "sleep"           ? "assistant.actions.focus-sleep"
        : mode == "dnd"             ? "assistant.actions.focus-dnd"
                                    : nullptr;
    if (!key) {
      finish(error("assistant.actions.focus-unavailable"));
      return;
    }
    result.text = i18n::tr(key);
    if (status["automatic"].get<bool>())
      result.text += " " + i18n::tr("assistant.actions.focus-automatic");
    finish(std::move(result));
    return;
  }
  case Kind::Focus:
    if (!m_ipc || m_ipc->execute("focus-set " + request.mode) != "ok\n") {
      finish(error("assistant.actions.failed"));
      return;
    }
    result.text = i18n::tr(
        request.mode == "off"          ? "assistant.actions.focus-off"
            : request.mode == "auto"   ? "assistant.actions.focus-auto"
            : request.mode == "gaming" ? "assistant.actions.focus-gaming"
            : request.mode == "sleep"  ? "assistant.actions.focus-sleep"
                                       : "assistant.actions.focus-work"
    );
    finish(std::move(result));
    return;
  case Kind::Invalid:
    return;
  }
  if (confirmed()) {
    finish(std::move(result));
    return;
  }
  m_poll.startRepeating(
      std::chrono::milliseconds(50),
      [this, confirmed = std::move(confirmed), result = std::move(result), failure, attempts = 0]() mutable {
        if (confirmed())
          finish(std::move(result));
        else if (++attempts >= 40)
          finish(error(failure));
      }
  );
}
