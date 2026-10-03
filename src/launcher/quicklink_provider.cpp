#include "launcher/quicklink_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>

namespace {

  // Result ids: "link:<id>" opens or searches a quicklink (the query rides in LauncherResult::query);
  // "web" is the search-the-web fallback.
  constexpr std::string_view kLinkPrefix = "link:";
  constexpr std::string_view kWebSearchId = "web";
  constexpr double kKeywordScore = 9000.0;
  constexpr double kFallbackScore = -10000.0;
  constexpr std::size_t kMinFallbackChars = 2;

  [[nodiscard]] bool takesQuery(const LauncherQuicklinkConfig& link) { return link.url.contains("{query}"); }

  [[nodiscard]] std::string glyphFor(const LauncherQuicklinkConfig& link) {
    if (!link.glyph.empty()) {
      return link.glyph;
    }
    return takesQuery(link) ? "world-search" : "link";
  }

  [[nodiscard]] std::string displayHost(std::string_view url) {
    std::string_view rest = url;
    if (const auto scheme = rest.find("://"); scheme != std::string_view::npos) {
      rest.remove_prefix(scheme + 3);
    }
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
      rest = rest.substr(0, slash);
    }
    if (rest.starts_with("www.")) {
      rest.remove_prefix(4);
    }
    return std::string(rest);
  }

  LauncherResult resultFor(const LauncherQuicklinkConfig& link, std::string_view query, double score) {
    LauncherResult result;
    result.id = std::string(kLinkPrefix) + link.id;
    if (!query.empty()) {
      result.query = std::string(query);
      result.title = i18n::tr("launcher.quicklinks.search-for", "name", link.name, "query", std::string(query));
    } else {
      result.title = link.name;
    }
    result.subtitle = link.keyword.empty() ? displayHost(link.url) : link.keyword;
    result.glyphName = glyphFor(link);
    result.kind = i18n::tr("launcher.kinds.quicklink");
    result.score = score;
    return result;
  }

} // namespace

QuicklinkProvider::QuicklinkProvider(ConfigService* config, ClipboardService* clipboard)
    : m_config(config), m_clipboard(clipboard) {}

std::string QuicklinkProvider::displayName() const { return i18n::tr("launcher.providers.quicklinks.title"); }

std::vector<LauncherQuicklinkConfig> QuicklinkProvider::effectiveQuicklinks(const ConfigService* config) {
  // Built-in links come first; a config link with the same id replaces one, others are added.
  std::vector<LauncherQuicklinkConfig> links = {
      {.id = "google",
       .name = "Google",
       .url = "https://www.google.com/search?q={query}",
       .keyword = "g",
       .glyph = "brand-google"},
      {.id = "duckduckgo",
       .name = "DuckDuckGo",
       .url = "https://duckduckgo.com/?q={query}",
       .keyword = "ddg",
       .glyph = "world-search"},
      {.id = "github",
       .name = "GitHub",
       .url = "https://github.com/search?q={query}",
       .keyword = "gh",
       .glyph = "brand-github"},
      {.id = "youtube",
       .name = "YouTube",
       .url = "https://www.youtube.com/results?search_query={query}",
       .keyword = "yt",
       .glyph = "brand-youtube"},
      {.id = "wikipedia",
       .name = "Wikipedia",
       .url = "https://en.wikipedia.org/w/index.php?search={query}",
       .keyword = "wiki",
       .glyph = "brand-wikipedia"},
      {.id = "archwiki",
       .name = "ArchWiki",
       .url = "https://wiki.archlinux.org/index.php?search={query}",
       .keyword = "aw",
       .glyph = "book"},
      {.id = "maps",
       .name = "Maps",
       .url = "https://www.openstreetmap.org/search?query={query}",
       .keyword = "maps",
       .glyph = "map"},
  };
  if (config == nullptr) {
    return links;
  }
  for (const auto& link : config->config().shell.launcher.quicklinks) {
    const auto it = std::ranges::find(links, link.id, &LauncherQuicklinkConfig::id);
    if (it != links.end()) {
      *it = link;
    } else {
      links.push_back(link);
    }
  }
  return links;
}

std::vector<LauncherResult>
QuicklinkProvider::match(const std::vector<LauncherQuicklinkConfig>& links, std::string_view text, bool listAll) {
  const std::string trimmed = StringUtils::trim(text);
  std::vector<LauncherResult> results;
  if (trimmed.empty()) {
    if (listAll) {
      for (const auto& link : links) {
        results.push_back(resultFor(link, {}, 0.0));
      }
    }
    return results;
  }

  // "<keyword> <query>" searches with that link straight away.
  const auto space = trimmed.find(' ');
  if (space != std::string::npos) {
    const std::string keyword = StringUtils::toLower(trimmed.substr(0, space));
    const std::string rest = StringUtils::trim(std::string_view(trimmed).substr(space + 1));
    for (const auto& link : links) {
      if (takesQuery(link) && !rest.empty() && StringUtils::toLower(link.keyword) == keyword) {
        results.push_back(resultFor(link, rest, kKeywordScore));
      }
    }
    if (!results.empty()) {
      return results;
    }
  }

  const std::string needle = StringUtils::toLower(trimmed);
  for (const auto& link : links) {
    const bool keywordHit = !link.keyword.empty() && StringUtils::toLower(link.keyword) == needle;
    const double score = keywordHit ? kKeywordScore : FuzzyMatch::score(needle, StringUtils::toLower(link.name));
    if (keywordHit || FuzzyMatch::isMatch(score)) {
      results.push_back(resultFor(link, {}, score));
    }
  }
  return results;
}

std::vector<LauncherResult> QuicklinkProvider::query(std::string_view text) const {
  auto results = match(effectiveQuicklinks(m_config), text, false);
  const std::string trimmed = StringUtils::trim(text);
  const bool keywordSearch = std::ranges::any_of(results, [](const LauncherResult& r) { return r.query.has_value(); });
  if (!keywordSearch
      && trimmed.size() >= kMinFallbackChars
      && m_config != nullptr
      && !m_config->config().shell.launcher.webSearchUrl.empty()) {
    LauncherResult web;
    web.id = std::string(kWebSearchId);
    web.query = trimmed;
    web.title = i18n::tr("launcher.quicklinks.search-web", "query", trimmed);
    web.subtitle = displayHost(m_config->config().shell.launcher.webSearchUrl);
    web.glyphName = "world-search";
    web.kind = i18n::tr("launcher.kinds.web-search");
    web.score = kFallbackScore;
    web.fallback = true;
    results.push_back(std::move(web));
  }
  return results;
}

std::vector<LauncherResult> QuicklinkProvider::queryPrefixed(std::string_view text) const {
  return match(effectiveQuicklinks(m_config), text, true);
}

std::optional<LauncherResult> QuicklinkProvider::resultForId(std::string_view resultId) const {
  for (const auto& link : effectiveQuicklinks(m_config)) {
    if (std::string(kLinkPrefix) + link.id == resultId) {
      return resultFor(link, {}, 0.0);
    }
  }
  return std::nullopt;
}

std::string QuicklinkProvider::urlFor(const LauncherResult& result) const {
  if (result.id == kWebSearchId) {
    return m_config != nullptr
        ? launcher_util::fillUrlTemplate(m_config->config().shell.launcher.webSearchUrl, result.query.value_or(""))
        : std::string();
  }
  for (const auto& link : effectiveQuicklinks(m_config)) {
    if (std::string(kLinkPrefix) + link.id == result.id) {
      return launcher_util::fillUrlTemplate(link.url, result.query.value_or(""));
    }
  }
  return {};
}

bool QuicklinkProvider::activate(const LauncherResult& result) {
  // A search link picked without a query: put its keyword in the field so the query can follow.
  if (result.id != kWebSearchId && !result.query.has_value()) {
    for (const auto& link : effectiveQuicklinks(m_config)) {
      if (std::string(kLinkPrefix) + link.id == result.id
          && takesQuery(link)
          && !link.keyword.empty()
          && m_requestQuery) {
        m_requestQuery(link.keyword + " ");
        return false;
      }
    }
  }
  return launcher_util::openUri(urlFor(result));
}

std::string QuicklinkProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.open-in-browser");
}

std::vector<LauncherAction> QuicklinkProvider::actions(const LauncherResult& /*result*/) const {
  return {{.id = "copy-url", .label = i18n::tr("launcher.actions.copy-url")}};
}

LauncherActionOutcome QuicklinkProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (actionId == "copy-url" && m_clipboard != nullptr) {
    return m_clipboard->copyText(urlFor(result)) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}
