#include "launcher/snippet_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
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

std::vector<SnippetProvider::Entry> SnippetProvider::entries() const {
  std::vector<Entry> out;
  if (m_config != nullptr) {
    for (const auto& snippet : m_config->config().shell.launcher.snippets) {
      out.push_back({
          .id = std::string(kConfigPrefix) + snippet.id,
          .name = snippet.name.empty() ? snippet.id : snippet.name,
          .keyword = snippet.keyword,
          .text = snippet.text,
      });
    }
  }
  if (m_store != nullptr) {
    for (const auto& snippet : m_store->snippets()) {
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

std::vector<LauncherResult> SnippetProvider::query(std::string_view text) const { return search(text, false); }

std::vector<LauncherResult> SnippetProvider::queryPrefixed(std::string_view text) const { return search(text, true); }

std::string SnippetProvider::expandedText(const Entry& entry) const {
  std::string clipboardText;
  if (entry.text.contains("{clipboard}") && m_clipboard != nullptr) {
    clipboardText = m_clipboard->clipboardText().value_or("");
  }
  return expand(entry.text, std::chrono::system_clock::now(), clipboardText);
}

bool SnippetProvider::activate(const LauncherResult& result) {
  const auto entry = entryFor(result.id);
  return entry.has_value() && m_clipboard != nullptr && m_clipboard->copyText(expandedText(*entry));
}

std::string SnippetProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  const bool paste = m_config == nullptr || m_config->config().shell.launcher.autoPaste != ClipboardAutoPasteMode::Off;
  return i18n::tr(paste ? "launcher.actions.paste-snippet" : "launcher.actions.copy-to-clipboard");
}

std::vector<LauncherAction> SnippetProvider::actions(const LauncherResult& result) const {
  std::vector<LauncherAction> actions;
  actions.push_back({.id = "copy", .label = i18n::tr("launcher.actions.copy-to-clipboard")});
  if (result.id.starts_with(kSavedPrefix)) {
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
  if (actionId == "delete" && m_store != nullptr && result.id.starts_with(kSavedPrefix)) {
    return m_store->remove(std::string_view(result.id).substr(kSavedPrefix.size())) ? LauncherActionOutcome::KeepOpen
                                                                                    : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> SnippetProvider::preview(const LauncherResult& result) const {
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
