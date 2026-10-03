#include "launcher/clipboard_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "launcher/snippet_provider.h"
#include "launcher/snippet_store.h"
#include "notification/notifications.h"
#include "time/time_format.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>

namespace {

  constexpr std::size_t kMaxResults = 200;
  constexpr std::size_t kMaxPreviewChars = 20000;
  constexpr std::size_t kSnippetNameChars = 40;

  [[nodiscard]] std::string firstLine(std::string_view text) {
    const std::string trimmed = StringUtils::trim(text);
    const auto newline = trimmed.find('\n');
    return newline == std::string::npos ? trimmed : StringUtils::trim(std::string_view(trimmed).substr(0, newline));
  }

  [[nodiscard]] bool looksLikeLink(std::string_view text) {
    const std::string trimmed = StringUtils::trim(text);
    return (trimmed.starts_with("http://") || trimmed.starts_with("https://"))
        && trimmed.find_first_of(" \n\t") == std::string::npos;
  }

  [[nodiscard]] std::size_t codePointCount(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
      return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
    }));
  }

  [[nodiscard]] std::string kindFor(const ClipboardEntry& entry) {
    if (entry.isImage()) {
      return i18n::tr("launcher.kinds.image");
    }
    if (looksLikeLink(entry.textPreview)) {
      return i18n::tr("launcher.kinds.link");
    }
    return i18n::tr("launcher.kinds.text");
  }

} // namespace

ClipboardProvider::ClipboardProvider(ClipboardService* clipboard, ConfigService* config, SnippetStore* snippets)
    : m_clipboard(clipboard), m_config(config), m_snippets(snippets) {}

std::string ClipboardProvider::displayName() const { return i18n::tr("launcher.providers.clipboard.title"); }

std::vector<LauncherResult> ClipboardProvider::query(std::string_view text) const {
  if (m_clipboard == nullptr) {
    return {};
  }
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  const auto& history = m_clipboard->history();
  std::vector<LauncherResult> results;
  for (std::size_t i = 0; i < history.size() && results.size() < kMaxResults; ++i) {
    const ClipboardEntry& entry = history[i];
    const bool image = entry.isImage();
    if (!needle.empty() && (image || !StringUtils::toLower(entry.textPreview).contains(needle))) {
      continue;
    }
    LauncherResult result;
    result.id = entry.storageId;
    result.title = image ? i18n::tr("launcher.clipboard.image", "size", launcher_util::formatByteSize(entry.byteSize))
                         : firstLine(entry.textPreview);
    result.subtitle = formatTimeAgo(entry.capturedAt);
    result.glyphName = image ? "photo" : (looksLikeLink(entry.textPreview) ? "link" : "file-text");
    result.kind = kindFor(entry);
    result.pinned = entry.pinned;
    // History is newest first; keep that order.
    result.score = -static_cast<double>(i);
    results.push_back(std::move(result));
  }
  return results;
}

std::optional<std::size_t> ClipboardProvider::indexFor(std::string_view storageId) const {
  if (m_clipboard == nullptr) {
    return std::nullopt;
  }
  const auto& history = m_clipboard->history();
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (history[i].storageId == storageId) {
      return i;
    }
  }
  return std::nullopt;
}

std::string ClipboardProvider::fullText(std::size_t index) const {
  if (m_clipboard == nullptr || !m_clipboard->ensureEntryLoaded(index)) {
    return {};
  }
  const ClipboardEntry& entry = m_clipboard->history()[index];
  if (entry.isImage()) {
    return {};
  }
  if (entry.data.empty()) {
    return entry.textPreview;
  }
  return std::string(entry.data.begin(), entry.data.end());
}

bool ClipboardProvider::copy(std::string_view storageId, bool promote) {
  const auto index = indexFor(storageId);
  if (!index.has_value() || !m_clipboard->ensureEntryLoaded(*index)) {
    return false;
  }
  const ClipboardEntry entry = m_clipboard->history()[*index];
  if (promote && !entry.pinned) {
    (void)m_clipboard->promoteEntry(*index);
  }
  return m_clipboard->copyEntry(entry);
}

bool ClipboardProvider::activate(const LauncherResult& result) { return copy(result.id, true); }

std::string ClipboardProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  const bool paste = m_config == nullptr || m_config->config().shell.launcher.autoPaste != ClipboardAutoPasteMode::Off;
  return i18n::tr(paste ? "launcher.actions.paste" : "launcher.actions.copy-to-clipboard");
}

std::vector<LauncherAction> ClipboardProvider::actions(const LauncherResult& result) const {
  std::vector<LauncherAction> actions;
  const auto index = indexFor(result.id);
  if (!index.has_value()) {
    return actions;
  }
  const ClipboardEntry& entry = m_clipboard->history()[*index];
  const bool paste = m_config == nullptr || m_config->config().shell.launcher.autoPaste != ClipboardAutoPasteMode::Off;
  if (paste) {
    actions.push_back({.id = "copy", .label = i18n::tr("launcher.actions.copy-to-clipboard")});
  }
  if (looksLikeLink(entry.textPreview)) {
    actions.push_back({.id = "open-link", .label = i18n::tr("launcher.actions.open-in-browser")});
  }
  actions.push_back(
      {.id = entry.pinned ? "unpin" : "pin",
       .label = i18n::tr(entry.pinned ? "launcher.actions.unpin" : "launcher.actions.pin")}
  );
  if (!entry.isImage() && m_snippets != nullptr) {
    actions.push_back({.id = "save-snippet", .label = i18n::tr("launcher.actions.save-as-snippet")});
  }
  actions.push_back({.id = "delete", .label = i18n::tr("launcher.actions.delete-entry")});
  return actions;
}

LauncherActionOutcome ClipboardProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  const auto index = indexFor(result.id);
  if (!index.has_value()) {
    return LauncherActionOutcome::Failed;
  }
  if (actionId == "copy") {
    return copy(result.id, true) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  if (actionId == "open-link") {
    return launcher_util::openUri(StringUtils::trim(m_clipboard->history()[*index].textPreview))
        ? LauncherActionOutcome::Done
        : LauncherActionOutcome::Failed;
  }
  if (actionId == "pin" || actionId == "unpin") {
    return m_clipboard->setEntryPinned(*index, actionId == "pin") ? LauncherActionOutcome::KeepOpen
                                                                  : LauncherActionOutcome::Failed;
  }
  if (actionId == "save-snippet" && m_snippets != nullptr) {
    const std::string text = fullText(*index);
    if (text.empty()) {
      return LauncherActionOutcome::Failed;
    }
    std::string name = firstLine(text);
    if (name.size() > kSnippetNameChars) {
      name = StringUtils::truncateUtf8CodePoints(name, kSnippetNameChars) + "…";
    }
    if (m_requestForm) {
      // Like Raycast, open the snippet form filled in so it can be named and given a keyword.
      m_requestForm(SnippetProvider::makeForm(m_config, m_snippets, {}, name, {}, text));
      return LauncherActionOutcome::KeepOpen;
    }
    m_snippets->add(name, text);
    notify::info("Noctalia", i18n::tr("launcher.snippets.saved"), name);
    return LauncherActionOutcome::Done;
  }
  if (actionId == "delete") {
    return m_clipboard->removeHistoryEntry(*index) ? LauncherActionOutcome::KeepOpen : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> ClipboardProvider::preview(const LauncherResult& result) const {
  const auto index = indexFor(result.id);
  if (!index.has_value() || !m_clipboard->ensureEntryLoaded(*index)) {
    return std::nullopt;
  }
  const ClipboardEntry& entry = m_clipboard->history()[*index];
  LauncherPreview preview;
  if (entry.isImage()) {
    preview.imageBytes = entry.data;
  } else {
    preview.body = fullText(*index);
    if (preview.body.size() > kMaxPreviewChars) {
      preview.body = StringUtils::truncateUtf8(preview.body, kMaxPreviewChars) + "…";
    }
  }
  preview.metadata.emplace_back(i18n::tr("launcher.preview.type"), kindFor(entry));
  if (entry.isImage()) {
    preview.metadata.emplace_back(i18n::tr("launcher.preview.size"), launcher_util::formatByteSize(entry.byteSize));
  } else {
    preview.metadata.emplace_back(
        i18n::tr("launcher.preview.characters"), std::to_string(codePointCount(preview.body))
    );
  }
  preview.metadata.emplace_back(i18n::tr("launcher.preview.copied"), formatTimeAgo(entry.capturedAt));
  return preview;
}
