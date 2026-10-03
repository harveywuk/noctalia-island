#include "launcher/quicklink_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "launcher/quicklink_store.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>

namespace {

  // Result ids: "link:<id>" opens or searches a quicklink (the query rides in LauncherResult::query);
  // "web" is the search-the-web fallback.
  constexpr std::string_view kLinkPrefix = "link:";
  constexpr std::string_view kWebSearchId = "web";
  constexpr std::string_view kCreateId = "create";
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

QuicklinkProvider::QuicklinkProvider(ConfigService* config, ClipboardService* clipboard, QuicklinkStore* store)
    : m_config(config), m_clipboard(clipboard), m_store(store) {}

std::string QuicklinkProvider::displayName() const { return i18n::tr("launcher.providers.quicklinks.title"); }

std::vector<LauncherQuicklinkConfig> QuicklinkProvider::builtinQuicklinks() {
  return {
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
}

std::vector<LauncherQuicklinkConfig>
QuicklinkProvider::effectiveQuicklinks(const ConfigService* config, QuicklinkStore* store) {
  // Built-in links come first. Config links, then links saved from the launcher, replace one with
  // the same id; others are added.
  std::vector<LauncherQuicklinkConfig> links = builtinQuicklinks();
  const auto merge = [&links](const std::vector<LauncherQuicklinkConfig>& extra) {
    for (const auto& link : extra) {
      const auto it = std::ranges::find(links, link.id, &LauncherQuicklinkConfig::id);
      if (it != links.end()) {
        *it = link;
      } else {
        links.push_back(link);
      }
    }
  };
  if (config != nullptr) {
    merge(config->config().shell.launcher.quicklinks);
  }
  if (store != nullptr) {
    merge(store->quicklinks());
  }
  return links;
}

std::vector<LauncherQuicklinkConfig> QuicklinkProvider::links() const { return effectiveQuicklinks(m_config, m_store); }

std::optional<LauncherQuicklinkConfig> QuicklinkProvider::linkFor(std::string_view resultId) const {
  if (!resultId.starts_with(kLinkPrefix)) {
    return std::nullopt;
  }
  for (auto& link : links()) {
    if (link.id == resultId.substr(kLinkPrefix.size())) {
      return link;
    }
  }
  return std::nullopt;
}

bool QuicklinkProvider::isSaved(std::string_view id) const {
  return m_store != nullptr && std::ranges::contains(m_store->quicklinks(), id, &LauncherQuicklinkConfig::id);
}

bool QuicklinkProvider::isEditable(std::string_view id) const {
  // A link written in config.toml is edited there; built-in and saved links are edited here.
  if (m_store == nullptr) {
    return false;
  }
  if (isSaved(id) || m_config == nullptr) {
    return true;
  }
  return !std::ranges::contains(m_config->config().shell.launcher.quicklinks, id, &LauncherQuicklinkConfig::id);
}

std::string QuicklinkProvider::normalizeUrl(std::string_view input) {
  std::string url = StringUtils::trim(input);
  if (url.empty() || url.contains(' ')) {
    return {};
  }
  if (!url.contains("://") && !url.starts_with("mailto:")) {
    url = "https://" + url;
  }
  const auto scheme = url.find("://");
  if (scheme != std::string::npos && scheme + 3 >= url.size()) {
    return {};
  }
  return url;
}

std::string QuicklinkProvider::validate(
    const std::vector<LauncherQuicklinkConfig>& links, std::string_view id, std::string_view url,
    std::string_view keyword
) {
  if (normalizeUrl(url).empty()) {
    return i18n::tr("launcher.forms.errors.invalid-url");
  }
  const std::string key = StringUtils::toLower(StringUtils::trim(keyword));
  if (key.contains(' ')) {
    return i18n::tr("launcher.forms.errors.keyword-spaces");
  }
  if (!key.empty()) {
    for (const auto& link : links) {
      if (link.id != id && StringUtils::toLower(link.keyword) == key) {
        return i18n::tr("launcher.forms.errors.keyword-taken", "keyword", key, "name", link.name);
      }
    }
  }
  return {};
}

LauncherForm QuicklinkProvider::form(const LauncherQuicklinkConfig* existing) const {
  LauncherForm form;
  form.title = i18n::tr(existing != nullptr ? "launcher.forms.edit-quicklink" : "launcher.forms.create-quicklink");
  form.submitLabel = i18n::tr("launcher.forms.save-quicklink");
  form.glyph = "link-plus";
  form.fields = {
      {.id = "name",
       .label = i18n::tr("launcher.forms.fields.name"),
       .placeholder = i18n::tr("launcher.forms.placeholders.quicklink-name"),
       .value = existing != nullptr ? existing->name : std::string(),
       .required = true},
      {.id = "url",
       .label = i18n::tr("launcher.forms.fields.link"),
       .placeholder = i18n::tr("launcher.forms.placeholders.quicklink-url"),
       .value = existing != nullptr ? existing->url : std::string(),
       .required = true},
      {.id = "keyword",
       .label = i18n::tr("launcher.forms.fields.keyword"),
       .placeholder = i18n::tr("launcher.forms.placeholders.quicklink-keyword"),
       .value = existing != nullptr ? existing->keyword : std::string()},
  };
  const std::string id = existing != nullptr ? existing->id : std::string();
  const std::string glyph = existing != nullptr ? existing->glyph : std::string();
  form.submit = [this, id, glyph](const std::vector<LauncherFormField>& fields) -> std::string {
    if (m_store == nullptr) {
      return {};
    }
    const std::string& url = fields[1].value;
    const std::string keyword = StringUtils::toLower(StringUtils::trim(fields[2].value));
    if (std::string error = validate(links(), id, url, keyword); !error.empty()) {
      return error;
    }
    m_store->put({
        .id = id,
        .name = StringUtils::trim(fields[0].value),
        .url = normalizeUrl(url),
        .keyword = keyword,
        .glyph = glyph,
    });
    return {};
  };
  return form;
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

std::optional<LauncherResult> QuicklinkProvider::createCommand(std::string_view text, bool listAll) const {
  if (m_store == nullptr || !m_requestForm) {
    return std::nullopt;
  }
  const std::string title = i18n::tr("launcher.forms.create-quicklink");
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  double score = 0.0;
  if (!needle.empty()) {
    if (!launcher_util::wordsMatch(
            needle, StringUtils::toLower(title + " " + i18n::tr("launcher.forms.quicklink-keywords"))
        )) {
      return std::nullopt;
    }
    score = FuzzyMatch::score(needle, StringUtils::toLower(title));
  } else if (!listAll) {
    return std::nullopt;
  }
  LauncherResult result;
  result.id = std::string(kCreateId);
  result.title = title;
  result.glyphName = "link-plus";
  result.kind = i18n::tr("launcher.kinds.command");
  result.score = score;
  return result;
}

std::vector<LauncherResult> QuicklinkProvider::query(std::string_view text) const {
  auto results = match(links(), text, false);
  if (auto create = createCommand(text, false); create.has_value()) {
    results.push_back(std::move(*create));
  }
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
  auto results = match(links(), text, true);
  if (auto create = createCommand(text, true); create.has_value()) {
    results.insert(results.begin(), std::move(*create));
  }
  return results;
}

std::optional<LauncherResult> QuicklinkProvider::resultForId(std::string_view resultId) const {
  for (const auto& link : links()) {
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
  for (const auto& link : links()) {
    if (std::string(kLinkPrefix) + link.id == result.id) {
      return launcher_util::fillUrlTemplate(link.url, result.query.value_or(""));
    }
  }
  return {};
}

bool QuicklinkProvider::activate(const LauncherResult& result) {
  if (result.id == kCreateId) {
    if (m_requestForm) {
      m_requestForm(form(nullptr));
    }
    return false; // the launcher stays open on the form
  }
  // A search link picked without a query: put its keyword in the field so the query can follow.
  if (result.id != kWebSearchId && !result.query.has_value()) {
    for (const auto& link : links()) {
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

std::string QuicklinkProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kCreateId) {
    return i18n::tr("launcher.forms.create-quicklink");
  }
  return i18n::tr("launcher.actions.open-in-browser");
}

std::vector<LauncherAction> QuicklinkProvider::actions(const LauncherResult& result) const {
  if (result.id == kCreateId) {
    return {};
  }
  std::vector<LauncherAction> actions{{.id = "copy-url", .label = i18n::tr("launcher.actions.copy-url")}};
  if (const auto link = linkFor(result.id); link.has_value() && m_requestForm) {
    if (isEditable(link->id)) {
      actions.push_back({.id = "edit", .label = i18n::tr("launcher.actions.edit-quicklink")});
    }
    if (isSaved(link->id)) {
      actions.push_back({.id = "delete", .label = i18n::tr("launcher.actions.delete-quicklink")});
    }
  }
  return actions;
}

LauncherActionOutcome QuicklinkProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (actionId == "edit" && m_requestForm) {
    const auto link = linkFor(result.id);
    if (!link.has_value() || !isEditable(link->id)) {
      return LauncherActionOutcome::Failed;
    }
    m_requestForm(form(&*link));
    return LauncherActionOutcome::KeepOpen;
  }
  if (actionId == "delete" && m_store != nullptr) {
    const auto link = linkFor(result.id);
    return link.has_value() && m_store->remove(link->id) ? LauncherActionOutcome::KeepOpen
                                                         : LauncherActionOutcome::Failed;
  }
  if (actionId == "copy-url" && m_clipboard != nullptr) {
    return m_clipboard->copyText(urlFor(result)) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}
