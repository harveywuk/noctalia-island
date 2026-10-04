#include "launcher/define_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "net/http_client.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace {

  constexpr auto kDebounce = std::chrono::milliseconds(350);
  constexpr std::size_t kMaxMeanings = 12;
  constexpr double kScore = 8000.0;
  constexpr std::string_view kMeaningPrefix = "meaning:";

  [[nodiscard]] bool isWordChar(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '-' || c == '\''; }

} // namespace

DefineProvider::DefineProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient)
    : m_clipboard(clipboard), m_config(config), m_httpClient(httpClient) {}

std::string DefineProvider::displayName() const { return i18n::tr("launcher.providers.dictionary.title"); }

std::string DefineProvider::wordFor(std::string_view text, bool prefixed) {
  std::string lower = StringUtils::toLower(StringUtils::trim(text));
  if (!prefixed) {
    bool matched = false;
    for (const std::string_view lead : {"define ", "def ", "meaning of ", "definition of "}) {
      if (lower.starts_with(lead)) {
        lower = StringUtils::trim(std::string_view(lower).substr(lead.size()));
        matched = true;
        break;
      }
    }
    if (!matched) {
      return {};
    }
  }
  if (lower.empty() || lower.size() > 40 || !std::ranges::all_of(lower, isWordChar)) {
    return {};
  }
  return lower;
}

std::string DefineProvider::urlFor(std::string_view word) {
  return "https://api.dictionaryapi.dev/api/v2/entries/en/" + launcher_util::urlEncode(word);
}

std::optional<DefineProvider::Entry> DefineProvider::parse(std::string_view json) {
  try {
    const auto parsed = nlohmann::json::parse(json);
    if (!parsed.is_array() || parsed.empty() || !parsed[0].is_object()) {
      return std::nullopt;
    }
    Entry entry;
    entry.word = parsed[0].value("word", "");
    entry.phonetic = parsed[0].value("phonetic", "");
    for (const auto& item : parsed) {
      if (entry.phonetic.empty() && item.contains("phonetics") && item["phonetics"].is_array()) {
        for (const auto& phonetic : item["phonetics"]) {
          const std::string text = phonetic.value("text", "");
          if (!text.empty()) {
            entry.phonetic = text;
            break;
          }
        }
      }
      if (!item.contains("meanings") || !item["meanings"].is_array()) {
        continue;
      }
      for (const auto& meaning : item["meanings"]) {
        const std::string partOfSpeech = meaning.value("partOfSpeech", "");
        if (!meaning.contains("definitions") || !meaning["definitions"].is_array()) {
          continue;
        }
        for (const auto& definition : meaning["definitions"]) {
          Meaning out{
              .partOfSpeech = partOfSpeech,
              .definition = definition.value("definition", ""),
              .example = definition.value("example", ""),
          };
          if (!out.definition.empty()) {
            entry.meanings.push_back(std::move(out));
          }
        }
      }
    }
    if (entry.word.empty() || entry.meanings.empty()) {
      return std::nullopt;
    }
    return entry;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool DefineProvider::isLoading() const { return !m_loadingWord.empty() || !m_pendingWord.empty(); }

void DefineProvider::reset() {
  m_debounce.stop();
  m_pendingWord.clear();
}

void DefineProvider::lookup(std::string word) const {
  if (m_httpClient == nullptr) {
    return;
  }
  m_pendingWord.clear();
  m_loadingWord = word;
  HttpRequest request;
  request.url = urlFor(word);
  request.followRedirects = true;
  m_httpClient->request(std::move(request), [this, word](HttpResponse response) {
    if (m_loadingWord != word) {
      return; // a newer lookup replaced it
    }
    m_loadingWord.clear();
    m_loadedWord = word;
    m_entry.reset();
    m_notFound = false;
    m_failed = false;
    if (response.transportOk && response.status == 200) {
      m_entry = parse(response.body);
      m_notFound = !m_entry.has_value();
    } else if (response.transportOk && response.status == 404) {
      m_notFound = true;
    } else {
      m_failed = true;
    }
    if (m_onChanged) {
      m_onChanged();
    }
  });
}

std::vector<LauncherResult> DefineProvider::resultsFor(std::string_view wordView) const {
  const std::string word(wordView);
  if (word.empty()) {
    return {};
  }
  const bool offline = m_config != nullptr && m_config->config().shell.offlineMode;
  if (word != m_loadedWord) {
    if (offline) {
      LauncherResult row;
      row.id = "offline";
      row.query = word;
      row.title = i18n::tr("launcher.dictionary.offline");
      row.subtitle = word;
      row.glyphName = "book";
      row.kind = i18n::tr("launcher.kinds.definition");
      row.score = kScore;
      return {std::move(row)};
    }
    if (word != m_loadingWord && word != m_pendingWord) {
      // Wait for a pause in typing before asking the service.
      m_pendingWord = word;
      m_debounce.start(kDebounce, [this]() {
        if (!m_pendingWord.empty()) {
          lookup(m_pendingWord);
        }
      });
    }
    LauncherResult row;
    row.id = "loading";
    row.query = word;
    row.title = i18n::tr("launcher.dictionary.looking-up", "word", word);
    row.subtitle = i18n::tr("launcher.dictionary.hint-subtitle");
    row.glyphName = "book";
    row.kind = i18n::tr("launcher.kinds.definition");
    row.score = kScore;
    return {std::move(row)};
  }
  std::vector<LauncherResult> results;
  if (m_entry.has_value()) {
    std::size_t index = 0;
    for (const Meaning& meaning : m_entry->meanings) {
      if (index >= kMaxMeanings) {
        break;
      }
      LauncherResult row;
      row.id = std::string(kMeaningPrefix) + std::to_string(index);
      row.title = meaning.definition;
      row.subtitle = meaning.partOfSpeech;
      if (index == 0 && !m_entry->phonetic.empty()) {
        row.subtitle = meaning.partOfSpeech + " · " + m_entry->phonetic;
      }
      row.glyphName = "book";
      row.kind = i18n::tr("launcher.kinds.definition");
      row.score = kScore - static_cast<double>(index);
      results.push_back(std::move(row));
      ++index;
    }
    return results;
  }
  LauncherResult row;
  row.id = "none";
  row.query = word;
  row.title =
      m_failed ? i18n::tr("launcher.dictionary.failed") : i18n::tr("launcher.dictionary.not-found", "word", word);
  row.glyphName = "book";
  row.kind = i18n::tr("launcher.kinds.definition");
  row.score = kScore;
  results.push_back(std::move(row));
  return results;
}

std::vector<LauncherResult> DefineProvider::query(std::string_view text) const {
  return resultsFor(wordFor(text, false));
}

std::vector<LauncherResult> DefineProvider::queryPrefixed(std::string_view text) const {
  const std::string word = wordFor(text, true);
  if (word.empty()) {
    if (StringUtils::isBlank(text)) {
      LauncherResult hint;
      hint.id = "hint";
      hint.title = i18n::tr("launcher.dictionary.hint");
      hint.subtitle = i18n::tr("launcher.dictionary.hint-subtitle");
      hint.glyphName = "book";
      hint.kind = i18n::tr("launcher.kinds.definition");
      return {std::move(hint)};
    }
    return {};
  }
  return resultsFor(word);
}

bool DefineProvider::activate(const LauncherResult& result) {
  if (result.id.starts_with(kMeaningPrefix)) {
    return m_clipboard != nullptr && m_clipboard->copyText(result.title);
  }
  // Status rows (looking up, not found, unreachable, offline) fall through to a web search.
  if (result.query.has_value() && m_config != nullptr) {
    const std::string& searchUrl = m_config->config().shell.launcher.webSearchUrl;
    if (!searchUrl.empty()) {
      return launcher_util::openUri(launcher_util::fillUrlTemplate(searchUrl, "define " + *result.query));
    }
  }
  return false;
}

std::string DefineProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id.starts_with(kMeaningPrefix)) {
    return i18n::tr("launcher.actions.copy-definition");
  }
  return i18n::tr(result.query.has_value() ? "launcher.quicklinks.search-web" : "launcher.actions.open");
}

std::vector<LauncherAction> DefineProvider::actions(const LauncherResult& result) const {
  if (!result.id.starts_with(kMeaningPrefix) || !m_entry.has_value()) {
    return {};
  }
  std::vector<LauncherAction> actions{{.id = "copy-word", .label = i18n::tr("launcher.actions.copy-word")}};
  const std::size_t index = static_cast<std::size_t>(std::stoul(result.id.substr(kMeaningPrefix.size())));
  if (index < m_entry->meanings.size() && !m_entry->meanings[index].example.empty()) {
    actions.push_back({.id = "copy-example", .label = i18n::tr("launcher.actions.copy-example")});
  }
  return actions;
}

LauncherActionOutcome DefineProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (m_clipboard == nullptr || !m_entry.has_value() || !result.id.starts_with(kMeaningPrefix)) {
    return LauncherActionOutcome::Failed;
  }
  if (actionId == "copy-word") {
    return m_clipboard->copyText(m_entry->word) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  if (actionId == "copy-example") {
    const std::size_t index = static_cast<std::size_t>(std::stoul(result.id.substr(kMeaningPrefix.size())));
    if (index >= m_entry->meanings.size()) {
      return LauncherActionOutcome::Failed;
    }
    return m_clipboard->copyText(m_entry->meanings[index].example) ? LauncherActionOutcome::Done
                                                                   : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}
