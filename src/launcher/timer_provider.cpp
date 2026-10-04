#include "launcher/timer_provider.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "notification/notifications.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <nlohmann/json.hpp>

namespace {

  constexpr Logger kLog("launcher-timer");
  constexpr std::string_view kRunningPrefix = "running:";
  constexpr std::string_view kStartId = "start";
  constexpr auto kMaxDuration = std::chrono::hours(24);
  constexpr double kStartScore = 9500.0;

  [[nodiscard]] std::optional<std::chrono::seconds> unitSeconds(std::string_view unit) {
    if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
      return std::chrono::hours(1);
    }
    if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") {
      return std::chrono::minutes(1);
    }
    if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds") {
      return std::chrono::seconds(1);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isFiller(std::string_view word) {
    return word == "timer"
        || word == "set"
        || word == "start"
        || word == "a"
        || word == "an"
        || word == "for"
        || word == "and"
        || word == "in";
  }

  // Splits "1h30m" / "1h30" / "10m" / "90" into number+unit pieces; a trailing bare number after a
  // unit means the next smaller unit (1h30 → 1 hour 30 minutes).
  [[nodiscard]] std::vector<std::pair<long, std::string>> pieces(std::string_view token) {
    std::vector<std::pair<long, std::string>> out;
    std::size_t i = 0;
    while (i < token.size()) {
      if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
        return {};
      }
      long value = 0;
      while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) {
        value = value * 10 + (token[i] - '0');
        ++i;
      }
      std::string unit;
      while (i < token.size() && std::isalpha(static_cast<unsigned char>(token[i]))) {
        unit.push_back(token[i]);
        ++i;
      }
      out.emplace_back(value, unit);
    }
    return out;
  }

} // namespace

TimerProvider::~TimerProvider() {
  for (auto& running : m_running) {
    if (m_ipc != nullptr) {
      (void)m_ipc->execute("island-activity-end " + running.id);
    }
  }
}

std::string TimerProvider::displayName() const { return i18n::tr("launcher.providers.timer.title"); }

std::optional<TimerProvider::Request> TimerProvider::parse(std::string_view text, bool prefixed) {
  const std::string lower = StringUtils::toLower(StringUtils::trim(text));
  if (lower.empty()) {
    return std::nullopt;
  }
  const auto words = StringUtils::split(lower, ' ');
  bool sawTimerWord = false;
  std::chrono::seconds total{0};
  bool sawDuration = false;
  std::string label;
  std::string lastUnit;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const std::string_view word = words[i];
    if (word.empty()) {
      continue;
    }
    if (word == "timer") {
      sawTimerWord = true;
      continue;
    }
    if (isFiller(word)) {
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(word.front()))) {
      auto parts = pieces(word);
      if (parts.empty()) {
        return std::nullopt;
      }
      // "10 min": the unit is the next word.
      if (parts.size() == 1 && parts[0].second.empty() && i + 1 < words.size() && unitSeconds(words[i + 1])) {
        parts[0].second = std::string(words[i + 1]);
        ++i;
      }
      for (auto& [value, unit] : parts) {
        std::optional<std::chrono::seconds> step;
        if (unit.empty()) {
          // A bare number: minutes, or the unit below the previous one ("1h30" → minutes).
          step = lastUnit == "h" ? std::chrono::seconds(std::chrono::minutes(1))
              : lastUnit == "m"  ? std::chrono::seconds(1)
                                 : std::chrono::seconds(std::chrono::minutes(1));
        } else {
          step = unitSeconds(unit);
          if (!step.has_value()) {
            return std::nullopt;
          }
          lastUnit = unit.substr(0, 1);
        }
        if (value <= 0) {
          return std::nullopt;
        }
        total += *step * value;
        sawDuration = true;
      }
      continue;
    }
    if (!label.empty()) {
      label += ' ';
    }
    label += word;
  }
  if (!sawDuration || total <= std::chrono::seconds(0) || total > kMaxDuration) {
    return std::nullopt;
  }
  if (!prefixed && !sawTimerWord) {
    return std::nullopt;
  }
  if (!label.empty()) {
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
  }
  return Request{.duration = total, .label = label};
}

std::string TimerProvider::formatRemaining(std::chrono::seconds remaining) {
  const long total = std::max<long>(0, remaining.count());
  const long hours = total / 3600;
  const long minutes = (total % 3600) / 60;
  const long seconds = total % 60;
  if (hours > 0) {
    return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
  }
  return std::format("{}:{:02}", minutes, seconds);
}

std::string TimerProvider::formatDuration(std::chrono::seconds duration) {
  const long total = duration.count();
  const long hours = total / 3600;
  const long minutes = (total % 3600) / 60;
  const long seconds = total % 60;
  std::string out;
  const auto add = [&out](long value, const char* key) {
    if (value <= 0) {
      return;
    }
    if (!out.empty()) {
      out += ' ';
    }
    out += i18n::trp(key, value);
  };
  add(hours, "launcher.timer.units.hour");
  add(minutes, "launcher.timer.units.minute");
  add(seconds, "launcher.timer.units.second");
  return out;
}

std::vector<LauncherResult> TimerProvider::runningResults() const {
  std::vector<LauncherResult> results;
  const auto now = std::chrono::steady_clock::now();
  for (const auto& running : m_running) {
    const auto remaining = std::chrono::ceil<std::chrono::seconds>(running.end - now);
    LauncherResult result;
    result.id = std::string(kRunningPrefix) + running.id;
    result.title = running.label.empty() ? i18n::tr("launcher.timer.running", "time", formatRemaining(remaining))
                                         : running.label + " · " + formatRemaining(remaining);
    result.subtitle = i18n::tr("launcher.timer.running-subtitle", "duration", formatDuration(running.total));
    result.glyphName = "hourglass";
    result.kind = i18n::tr("launcher.kinds.timer");
    result.score = 100.0;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> TimerProvider::query(std::string_view text) const {
  const auto request = parse(text, false);
  if (!request.has_value()) {
    return {};
  }
  LauncherResult result;
  result.id = std::string(kStartId);
  result.title = i18n::tr("launcher.timer.start", "duration", formatDuration(request->duration));
  result.subtitle = request->label;
  result.glyphName = "hourglass";
  result.kind = i18n::tr("launcher.kinds.command");
  result.query = StringUtils::trim(text);
  result.score = kStartScore;
  return {std::move(result)};
}

std::vector<LauncherResult> TimerProvider::queryPrefixed(std::string_view text) const {
  std::vector<LauncherResult> results = runningResults();
  if (const auto request = parse(text, true); request.has_value()) {
    LauncherResult result;
    result.id = std::string(kStartId);
    result.title = i18n::tr("launcher.timer.start", "duration", formatDuration(request->duration));
    result.subtitle = request->label;
    result.glyphName = "hourglass";
    result.kind = i18n::tr("launcher.kinds.command");
    result.query = StringUtils::trim(text);
    result.score = kStartScore;
    results.insert(results.begin(), std::move(result));
  } else if (StringUtils::isBlank(text) && results.empty()) {
    LauncherResult hint;
    hint.id = "hint";
    hint.title = i18n::tr("launcher.timer.hint");
    hint.subtitle = i18n::tr("launcher.timer.hint-subtitle");
    hint.glyphName = "hourglass";
    hint.kind = i18n::tr("launcher.kinds.command");
    results.push_back(std::move(hint));
  }
  return results;
}

std::string TimerProvider::title(const Running& running) const {
  const auto remaining = std::chrono::ceil<std::chrono::seconds>(running.end - std::chrono::steady_clock::now());
  return running.label.empty() ? formatRemaining(remaining) : running.label + " · " + formatRemaining(remaining);
}

void TimerProvider::start(const Request& request) {
  Running running;
  running.id = "launcher-timer-" + std::to_string(m_nextId++);
  running.label = request.label;
  running.total = request.duration;
  running.end = std::chrono::steady_clock::now() + request.duration;
  running.tick = std::make_unique<Timer>();
  const std::string id = running.id;
  m_running.push_back(std::move(running));
  if (m_ipc != nullptr) {
    const nlohmann::json start = {
        {"id", id}, {"title", title(m_running.back())}, {"icon", "hourglass"}, {"progress", 100}
    };
    const std::string reply = m_ipc->execute("island-activity-start " + start.dump());
    if (reply.starts_with("error")) {
      kLog.warn("island activity: {}", StringUtils::trim(reply));
    }
  }
  m_running.back().tick->startRepeating(std::chrono::seconds(1), [this, id]() { tick(id); });
}

void TimerProvider::tick(const std::string& id) {
  const auto it = std::ranges::find(m_running, id, &Running::id);
  if (it == m_running.end()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= it->end) {
    finish(id);
    return;
  }
  if (m_ipc != nullptr) {
    const auto remaining = std::chrono::duration<double>(it->end - now).count();
    const int percent =
        static_cast<int>(std::clamp(remaining / static_cast<double>(it->total.count()) * 100.0, 0.0, 100.0));
    (void)m_ipc->execute(std::format("island-activity-update {} {} {}", id, percent, title(*it)));
  }
}

void TimerProvider::finish(const std::string& id) {
  const auto it = std::ranges::find(m_running, id, &Running::id);
  if (it == m_running.end()) {
    return;
  }
  const std::string label = it->label;
  const std::string duration = formatDuration(it->total);
  it->tick->stop();
  m_running.erase(it);
  if (m_ipc != nullptr) {
    (void)m_ipc->execute("island-activity-end " + id);
  }
  notify::info(
      i18n::tr("launcher.providers.timer.title"),
      label.empty() ? i18n::tr("launcher.timer.done") : i18n::tr("launcher.timer.done-label", "label", label),
      i18n::tr("launcher.timer.done-body", "duration", duration)
  );
}

void TimerProvider::cancel(const std::string& id) {
  const auto it = std::ranges::find(m_running, id, &Running::id);
  if (it == m_running.end()) {
    return;
  }
  it->tick->stop();
  m_running.erase(it);
  if (m_ipc != nullptr) {
    (void)m_ipc->execute("island-activity-end " + id);
  }
}

bool TimerProvider::activate(const LauncherResult& result) {
  if (result.id == kStartId) {
    const auto request = parse(result.query.value_or(std::string()), true);
    if (!request.has_value()) {
      return false;
    }
    start(*request);
    return true;
  }
  // A running timer: Return leaves it alone and closes, like Raycast's timer list.
  return result.id.starts_with(kRunningPrefix);
}

std::string TimerProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kStartId) {
    return i18n::tr("launcher.actions.start-timer");
  }
  return i18n::tr("launcher.actions.open");
}

std::vector<LauncherAction> TimerProvider::actions(const LauncherResult& result) const {
  if (!result.id.starts_with(kRunningPrefix)) {
    return {};
  }
  return {
      {.id = "cancel", .label = i18n::tr("launcher.actions.cancel-timer")},
      {.id = "add-minute", .label = i18n::tr("launcher.actions.add-minute")},
  };
}

LauncherActionOutcome TimerProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (!result.id.starts_with(kRunningPrefix)) {
    return LauncherActionOutcome::Failed;
  }
  const std::string id = result.id.substr(kRunningPrefix.size());
  if (actionId == "cancel") {
    cancel(id);
    return LauncherActionOutcome::KeepOpen;
  }
  if (actionId == "add-minute") {
    const auto it = std::ranges::find(m_running, id, &Running::id);
    if (it == m_running.end()) {
      return LauncherActionOutcome::Failed;
    }
    it->end += std::chrono::minutes(1);
    it->total += std::chrono::minutes(1);
    tick(id);
    return LauncherActionOutcome::KeepOpen;
  }
  return LauncherActionOutcome::Failed;
}
