#include "launcher/snippet_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "launcher/snippet_store.h"
#include "time/time_format.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <ctime>

namespace {

  // Ids: "config:<table key>" for config snippets, "saved:<id>" for ones saved from the launcher.
  constexpr std::string_view kConfigPrefix = "config:";
  constexpr std::string_view kSavedPrefix = "saved:";
  constexpr std::string_view kCreateId = "create";
  constexpr double kKeywordScore = 9000.0;

  [[nodiscard]] std::string oneLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      out.push_back(c == '\n' || c == '\t' || c == '\r' ? ' ' : c);
    }
    return StringUtils::trim(out);
  }

  void replaceAll(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
      text.replace(pos, from.size(), to);
    }
  }

} // namespace

SnippetProvider::SnippetProvider(ConfigService* config, ClipboardService* clipboard, SnippetStore* store)
    : m_config(config), m_clipboard(clipboard), m_store(store) {}

std::string SnippetProvider::displayName() const { return i18n::tr("launcher.providers.snippets.title"); }

std::string SnippetProvider::expand(
    std::string_view text, std::chrono::system_clock::time_point now, std::string_view clipboardText
) {
  std::string out(text);
  if (!out.contains('{')) {
    return out;
  }
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&seconds, &local);
  replaceAll(out, "{datetime}", formatStrftime("%Y-%m-%d %H:%M", local));
  replaceAll(out, "{date}", formatStrftime("%Y-%m-%d", local));
  replaceAll(out, "{time}", formatStrftime("%H:%M", local));
  replaceAll(out, "{clipboard}", clipboardText);
  return out;
}

std::vector<SnippetProvider::Entry> SnippetProvider::entries() const { return collect(m_config, m_store); }

std::vector<SnippetProvider::Entry> SnippetProvider::collect(const ConfigService* config, SnippetStore* store) {
  std::vector<Entry> out;
  if (config != nullptr) {
    for (const auto& snippet : config->config().shell.launcher.snippets) {
      out.push_back({
          .id = std::string(kConfigPrefix) + snippet.id,
          .name = snippet.name.empty() ? snippet.id : snippet.name,
          .keyword = snippet.keyword,
          .text = snippet.text,
      });
    }
  }
  if (store != nullptr) {
    for (const auto& snippet : store->snippets()) {
      out.push_back({
          .id = std::string(kSavedPrefix) + snippet.id,
          .name = snippet.name,
          .keyword = snippet.keyword,
          .text = snippet.text,
          .saved = true,
      });
    }
  }
  return out;
}

std::string SnippetProvider::validate(
    ConfigService* config, SnippetStore* store, std::string_view editingId, std::string_view keyword
) {
  const std::string key = StringUtils::toLower(StringUtils::trim(keyword));
  if (key.contains(' ')) {
    return i18n::tr("launcher.forms.errors.keyword-spaces");
  }
  if (!key.empty()) {
    for (const auto& entry : collect(config, store)) {
      if (entry.id != editingId && StringUtils::toLower(entry.keyword) == key) {
        return i18n::tr("launcher.forms.errors.keyword-taken", "keyword", key, "name", entry.name);
      }
    }
  }
  return {};
}

LauncherForm SnippetProvider::makeForm(
    ConfigService* config, SnippetStore* store, std::string savedId, std::string name, std::string keyword,
    std::string text
) {
  LauncherForm form;
  form.title = i18n::tr(savedId.empty() ? "launcher.forms.create-snippet" : "launcher.forms.edit-snippet");
  form.submitLabel = i18n::tr("launcher.forms.save-snippet");
  form.glyph = "notes";
  form.fields = {
      {.id = "name",
       .label = i18n::tr("launcher.forms.fields.name"),
       .placeholder = i18n::tr("launcher.forms.placeholders.snippet-name"),
       .value = std::move(name),
       .required = true},
      {.id = "text",
       .label = i18n::tr("launcher.forms.fields.text"),
       .placeholder = i18n::tr("launcher.forms.placeholders.snippet-text"),
       .value = std::move(text),
       .required = true,
       .multiline = true},
      {.id = "keyword",
       .label = i18n::tr("launcher.forms.fields.keyword"),
       .placeholder = i18n::tr("launcher.forms.placeholders.snippet-keyword"),
       .value = std::move(keyword)},
  };
  form.submit = [config, store, savedId](const std::vector<LauncherFormField>& fields) -> std::string {
    if (store == nullptr) {
      return {};
    }
    const std::string editingId = savedId.empty() ? std::string() : std::string(kSavedPrefix) + savedId;
    if (std::string error = validate(config, store, editingId, fields[2].value); !error.empty()) {
      return error;
    }
    std::string name = StringUtils::trim(fields[0].value);
    std::string keyword = StringUtils::toLower(StringUtils::trim(fields[2].value));
    if (savedId.empty()) {
      (void)store->add(std::move(name), fields[1].value, std::move(keyword));
    } else {
      (void)store->update(savedId, std::move(name), fields[1].value, std::move(keyword));
    }
    return {};
  };
  return form;
}

std::optional<LauncherResult> SnippetProvider::createCommand(std::string_view text, bool listAll) const {
  if (m_store == nullptr || !m_requestForm) {
    return std::nullopt;
  }
  const std::string title = i18n::tr("launcher.forms.create-snippet");
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  double score = 0.0;
  if (!needle.empty()) {
    const std::string haystack = StringUtils::toLower(title + " " + i18n::tr("launcher.forms.snippet-keywords"));
    if (!launcher_util::wordsMatch(needle, haystack)) {
      return std::nullopt;
    }
    score = FuzzyMatch::score(needle, StringUtils::toLower(title));
  } else if (!listAll) {
    return std::nullopt;
  }
  LauncherResult result;
  result.id = std::string(kCreateId);
  result.title = title;
  result.glyphName = "file-plus";
  result.kind = i18n::tr("launcher.kinds.command");
  result.score = score;
  return result;
}

std::optional<SnippetProvider::Entry> SnippetProvider::entryFor(std::string_view resultId) const {
  for (auto& entry : entries()) {
    if (entry.id == resultId) {
      return entry;
    }
  }
  return std::nullopt;
}

std::vector<LauncherResult> SnippetProvider::search(std::string_view text, bool listAll) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  if (needle.empty() && !listAll) {
    return {};
  }
  std::vector<LauncherResult> results;
  for (const auto& entry : entries()) {
    double score = 0.0;
    if (!needle.empty()) {
      if (!entry.keyword.empty() && StringUtils::toLower(entry.keyword) == needle) {
        score = kKeywordScore;
      } else {
        score = FuzzyMatch::score(needle, StringUtils::toLower(entry.name));
        if (!FuzzyMatch::isMatch(score)) {
          continue;
        }
      }
    }
    LauncherResult result;
    result.id = entry.id;
    result.title = entry.name;
    result.subtitle = oneLine(entry.text);
    result.glyphName = "notes";
    result.kind = i18n::tr("launcher.kinds.snippet");
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> SnippetProvider::query(std::string_view text) const {
  auto results = search(text, false);
  if (auto create = createCommand(text, false); create.has_value()) {
    results.push_back(std::move(*create));
  }
  return results;
}

std::vector<LauncherResult> SnippetProvider::queryPrefixed(std::string_view text) const {
  auto results = search(text, true);
  if (auto create = createCommand(text, true); create.has_value()) {
    results.insert(results.begin(), std::move(*create));
  }
  return results;
}

std::string SnippetProvider::expandedText(const Entry& entry) const {
  std::string clipboardText;
  if (entry.text.contains("{clipboard}") && m_clipboard != nullptr) {
    clipboardText = m_clipboard->clipboardText().value_or("");
  }
  return expand(entry.text, std::chrono::system_clock::now(), clipboardText);
}

bool SnippetProvider::activate(const LauncherResult& result) {
  if (result.id == kCreateId) {
    if (m_requestForm) {
      m_requestForm(makeForm(m_config, m_store, {}, {}, {}, {}));
    }
    return false; // the launcher stays open on the form
  }
  const auto entry = entryFor(result.id);
  return entry.has_value() && m_clipboard != nullptr && m_clipboard->copyText(expandedText(*entry));
}

std::string SnippetProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kCreateId) {
    return i18n::tr("launcher.forms.create-snippet");
  }
  const bool paste = m_config == nullptr || m_config->config().shell.launcher.autoPaste != ClipboardAutoPasteMode::Off;
  return i18n::tr(paste ? "launcher.actions.paste-snippet" : "launcher.actions.copy-to-clipboard");
}

std::vector<LauncherAction> SnippetProvider::actions(const LauncherResult& result) const {
  std::vector<LauncherAction> actions;
  if (result.id == kCreateId) {
    return actions;
  }
  actions.push_back({.id = "copy", .label = i18n::tr("launcher.actions.copy-to-clipboard")});
  if (result.id.starts_with(kSavedPrefix)) {
    if (m_requestForm) {
      actions.push_back({.id = "edit", .label = i18n::tr("launcher.actions.edit-snippet")});
    }
    actions.push_back({.id = "delete", .label = i18n::tr("launcher.actions.delete-snippet")});
  }
  return actions;
}

LauncherActionOutcome SnippetProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (actionId == "copy") {
    const auto entry = entryFor(result.id);
    return entry.has_value() && m_clipboard != nullptr && m_clipboard->copyText(expandedText(*entry))
        ? LauncherActionOutcome::Done
        : LauncherActionOutcome::Failed;
  }
  if (actionId == "edit" && m_requestForm && result.id.starts_with(kSavedPrefix)) {
    const auto entry = entryFor(result.id);
    if (!entry.has_value()) {
      return LauncherActionOutcome::Failed;
    }
    m_requestForm(
        makeForm(m_config, m_store, result.id.substr(kSavedPrefix.size()), entry->name, entry->keyword, entry->text)
    );
    return LauncherActionOutcome::KeepOpen;
  }
  if (actionId == "delete" && m_store != nullptr && result.id.starts_with(kSavedPrefix)) {
    return m_store->remove(std::string_view(result.id).substr(kSavedPrefix.size())) ? LauncherActionOutcome::KeepOpen
                                                                                    : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> SnippetProvider::preview(const LauncherResult& result) const {
  if (result.id == kCreateId) {
    LauncherPreview preview;
    preview.title = result.title;
    preview.body = i18n::tr("launcher.forms.create-snippet-help");
    return preview;
  }
  const auto entry = entryFor(result.id);
  if (!entry.has_value()) {
    return std::nullopt;
  }
  LauncherPreview preview;
  preview.body = entry->text;
  preview.metadata.emplace_back(i18n::tr("launcher.preview.name"), entry->name);
  if (!entry->keyword.empty()) {
    preview.metadata.emplace_back(i18n::tr("launcher.preview.keyword"), entry->keyword);
  }
  preview.metadata.emplace_back(
      i18n::tr("launcher.preview.source"),
      i18n::tr(entry->saved ? "launcher.snippets.source-saved" : "launcher.snippets.source-config")
  );
  return preview;
}
