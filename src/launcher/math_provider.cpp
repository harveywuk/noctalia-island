#include "launcher/math_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/date_provider.h"
#include "launcher/time_provider.h"
#include "launcher/timer_provider.h"
#include "net/http_client.h"
#include "util/file_utils.h"
#include "wayland/clipboard_service.h"

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <libqalculate/Calculator.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>

namespace {

  // Cheap pre-filter: the provider participates in global search on every keystroke,
  // so reject anything without a digit to avoid evaluating plain search text
  // (e.g. "firefox"). Letters/spaces are kept so unit and currency conversions
  // like "10 cm to in" or "5 USD to EUR" still reach libqalculate.
  bool looksLikeMath(std::string_view text) {
    for (char c : text) {
      if (c >= '0' && c <= '9') {
        return true;
      }
    }
    return false;
  }

  std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
      ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
      --end;
    }
    return std::string(text.substr(begin, end - begin));
  }

  // Raycast-style conversions say "in" ("5 ft in cm", "100 usd in eur"); libqalculate reads "in" as
  // inches, so the last " in " becomes its conversion operator. Temperature shorthand
  // ("20c in f") is spelled out, since bare c and f are the speed of light and farads.
  std::string conversionSyntax(std::string_view text) {
    static const std::regex kTemperature(
        R"(^\s*(-?[0-9.,]+)\s*(?:°)?\s*([cfk])\s+(?:in|to)\s+(?:°)?\s*([cfk])\s*$)", std::regex::icase
    );
    const std::string input(text);
    std::smatch match;
    if (std::regex_match(input, match, kTemperature)) {
      const auto unit = [](const std::string& letter) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(letter.front())));
        return c == 'c' ? std::string("oC") : (c == 'f' ? std::string("oF") : std::string("K"));
      };
      return match[1].str() + " " + unit(match[2].str()) + " to " + unit(match[3].str());
    }
    std::string out = input;
    if (const auto in = out.rfind(" in "); in != std::string::npos && in > 0) {
      out.replace(in, 4, " to ");
    }
    return out;
  }

  bool shouldRefreshExchangeRateSource(std::string_view url) { return !url.contains("nbrb.by"); }

  constexpr std::size_t kMaxHistory = 30;
  constexpr std::string_view kHistoryPrefix = "history:";
  constexpr std::string_view kClearHistoryId = "clear-history";

} // namespace

MathProvider::MathProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient)
    : m_clipboard(clipboard), m_config(config), m_httpClient(httpClient) {}

MathProvider::~MathProvider() = default;

std::string MathProvider::displayName() const { return i18n::tr("launcher.providers.calculator.title"); }

void MathProvider::initialize() {
  m_calc = std::make_unique<Calculator>();
  // Load any cached rates before definitions so currency units pick them up.
  m_calc->loadExchangeRates();
  m_calc->loadGlobalDefinitions();
  // Pre-warm internal state (GMP randstate) so the destructor path is safe.
  m_calc->calculate("0");
  m_calc->clearMessages();
  refreshExchangeRates();
}

void MathProvider::refreshExchangeRates() {
  if (m_httpClient == nullptr || m_config == nullptr || !m_calc) {
    return;
  }
  if (m_config->config().shell.offlineMode
      || !m_config->config().shell.launcher.fetchExchangeRates
      || !m_calc->canFetch()) {
    return;
  }

  std::vector<std::pair<std::string, std::filesystem::path>> sources;
  for (int i = 1;; ++i) {
    std::string url = m_calc->getExchangeRatesUrl(i);
    std::string file = m_calc->getExchangeRatesFileName(i);
    if (url.empty() || file.empty()) {
      break;
    }
    if (!shouldRefreshExchangeRateSource(url)) {
      continue;
    }
    sources.emplace_back(std::move(url), std::filesystem::path(std::move(file)));
  }
  if (sources.empty()) {
    return;
  }

  auto remaining = std::make_shared<int>(static_cast<int>(sources.size()));
  for (auto& [url, path] : sources) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    m_httpClient->download(url, path, [this, remaining](bool) {
      if (--(*remaining) == 0) {
        // All sources settled; reload whatever landed on disc.
        m_calc->loadExchangeRates();
      }
    });
  }
}

std::vector<LauncherResult> MathProvider::query(std::string_view text) const {
  if (!looksLikeMath(text)) {
    return {};
  }
  // Timers and dates ("timer 10m tea", "3 days from now") read as unit expressions to libqalculate.
  if (TimerProvider::parse(text, false).has_value()) {
    return {};
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const std::chrono::year_month_day today{
      std::chrono::year{local.tm_year + 1900}, std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)},
      std::chrono::day{static_cast<unsigned>(local.tm_mday)}
  };
  if (DateProvider::answer(text, today, false).has_value()) {
    return {};
  }
  return evaluate(text, false);
}

std::vector<LauncherResult> MathProvider::queryPrefixed(std::string_view text) const {
  if (trimmed(text).empty()) {
    return historyResults();
  }
  return evaluate(text, true);
}

std::deque<MathProvider::HistoryEntry> MathProvider::loadHistory(const std::string& path) {
  std::deque<HistoryEntry> history;
  std::ifstream file(path);
  if (!file.is_open()) {
    return history;
  }
  try {
    const auto json = nlohmann::json::parse(file);
    for (const auto& item : json) {
      HistoryEntry entry{.expression = item.value("expression", ""), .result = item.value("result", "")};
      if (!entry.result.empty()) {
        history.push_back(std::move(entry));
      }
    }
  } catch (const nlohmann::json::exception&) {
    history.clear();
  }
  return history;
}

void MathProvider::saveHistory(const std::string& path, const std::deque<HistoryEntry>& history) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  nlohmann::json json = nlohmann::json::array();
  for (const auto& entry : history) {
    json.push_back({{"expression", entry.expression}, {"result", entry.result}});
  }
  std::ofstream file(path, std::ios::trunc);
  file << json.dump(2) << '\n';
}

std::string MathProvider::historyPath() const {
  const std::string dir = FileUtils::stateDir();
  return (dir.empty() ? "." : dir) + "/calculator_history.json";
}

void MathProvider::remember(std::string expression, std::string result) {
  if (!m_history.has_value()) {
    m_history = loadHistory(historyPath());
  }
  std::erase_if(*m_history, [&](const HistoryEntry& entry) {
    return entry.expression == expression && entry.result == result;
  });
  m_history->push_front({.expression = std::move(expression), .result = std::move(result)});
  while (m_history->size() > kMaxHistory) {
    m_history->pop_back();
  }
  saveHistory(historyPath(), *m_history);
}

std::vector<LauncherResult> MathProvider::historyResults() const {
  if (!m_history.has_value()) {
    m_history = loadHistory(historyPath());
  }
  std::vector<LauncherResult> results;
  for (std::size_t i = 0; i < m_history->size(); ++i) {
    const HistoryEntry& entry = (*m_history)[i];
    LauncherResult r;
    r.id = std::string(kHistoryPrefix) + std::to_string(i);
    r.title = "= " + entry.result;
    r.subtitle = entry.expression;
    r.glyphName = "history";
    r.kind = i18n::tr("launcher.calculator.history");
    r.score = static_cast<double>(m_history->size() - i);
    results.push_back(std::move(r));
  }
  if (!results.empty()) {
    LauncherResult clear;
    clear.id = std::string(kClearHistoryId);
    clear.title = i18n::tr("launcher.calculator.clear-history");
    clear.glyphName = "trash";
    clear.kind = i18n::tr("launcher.kinds.command");
    clear.score = 0.0;
    results.push_back(std::move(clear));
  }
  return results;
}

std::vector<LauncherResult> MathProvider::evaluate(std::string_view text, bool prefixed) const {
  // Times and time zones ("3pm in tokyo") belong to the Time provider.
  if (TimeProvider::parse(text, false).has_value()) {
    return {};
  }
  const std::string localized = conversionSyntax(trimmed(text));
  if (!m_calc || localized.empty()) {
    return {};
  }

  // Drop any messages left over from a previous evaluation.
  m_calc->clearMessages();

  EvaluationOptions eo = default_user_evaluation_options;
  eo.parse_options.dot_as_separator = m_calc->default_dot_as_separator;
  PrintOptions po = default_print_options;
  po.use_unicode_signs = false;
  // Collapse interval arithmetic to a single rounded value instead of "interval(a, b)".
  po.interval_display = INTERVAL_DISPLAY_SIGNIFICANT_DIGITS;

  // libqalculate expects '.' decimals internally; convert locale signs first.
  const std::string input = m_calc->unlocalizeExpression(localized, eo.parse_options);
  if (input.empty()) {
    return {};
  }

  std::string output = m_calc->calculateAndPrint(input, /*msecs=*/200, eo, po);

  bool hadError = false;
  bool hadWarning = false;
  for (CalculatorMessage* m = m_calc->message(); m != nullptr; m = m_calc->nextMessage()) {
    if (m->type() == MESSAGE_ERROR) {
      hadError = true;
    } else if (m->type() == MESSAGE_WARNING) {
      hadWarning = true;
    }
  }

  // Reject errors and no-ops (e.g. the user just typed a bare number). In the global search also
  // reject answers built from words libqalculate didn't know (it quotes them: 'tea'), which is
  // what a plain phrase with a number in it produces.
  if (hadError || output.empty() || output == input || output == localized) {
    return {};
  }
  if (!prefixed && (hadWarning || output.contains('\''))) {
    return {};
  }

  LauncherResult r;
  r.id = "math";
  r.title = "= " + output;
  r.subtitle = std::string(text);
  r.glyphName = "calculator";
  r.score = 10000;

  return {std::move(r)};
}

bool MathProvider::activate(const LauncherResult& result) {
  if (result.id == kClearHistoryId) {
    m_history = std::deque<HistoryEntry>{};
    saveHistory(historyPath(), *m_history);
    return false; // the list refreshes through the actions path; Return here keeps the launcher open
  }
  if (result.id != "math" && !result.id.starts_with(kHistoryPrefix)) {
    return false;
  }

  std::string value = result.title.substr(2);
  if (result.id == "math") {
    remember(result.subtitle, value);
  }
  return m_clipboard != nullptr && m_clipboard->copyText(std::move(value));
}

std::string MathProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kClearHistoryId) {
    return i18n::tr("launcher.calculator.clear-history");
  }
  const bool paste = m_config == nullptr || m_config->config().shell.launcher.autoPaste != ClipboardAutoPasteMode::Off;
  return i18n::tr(paste ? "launcher.actions.paste-answer" : "launcher.actions.copy-answer");
}

std::vector<LauncherAction> MathProvider::actions(const LauncherResult& result) const {
  if (result.id == kClearHistoryId) {
    return {};
  }
  std::vector<LauncherAction> actions{
      {.id = "copy-answer", .label = i18n::tr("launcher.actions.copy-answer")},
      {.id = "copy-expression", .label = i18n::tr("launcher.actions.copy-expression")},
  };
  if (result.id.starts_with(kHistoryPrefix)) {
    actions.push_back({.id = "clear-history", .label = i18n::tr("launcher.calculator.clear-history")});
  }
  return actions;
}

LauncherActionOutcome MathProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (actionId == "clear-history") {
    m_history = std::deque<HistoryEntry>{};
    saveHistory(historyPath(), *m_history);
    return LauncherActionOutcome::KeepOpen;
  }
  if (m_clipboard == nullptr || (result.id != "math" && !result.id.starts_with(kHistoryPrefix))) {
    return LauncherActionOutcome::Failed;
  }
  std::string value = actionId == "copy-expression" ? result.subtitle : result.title.substr(2);
  if (result.id == "math") {
    remember(result.subtitle, result.title.substr(2));
  }
  return m_clipboard->copyText(std::move(value)) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
}
