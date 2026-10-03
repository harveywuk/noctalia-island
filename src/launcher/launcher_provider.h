#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher {
  inline constexpr std::array kBuiltinProviders = {std::string_view("calculator"), std::string_view("clipboard"),
                                                   std::string_view("date"),       std::string_view("emoji"),
                                                   std::string_view("files"),      std::string_view("panels"),
                                                   std::string_view("processes"),  std::string_view("quicklinks"),
                                                   std::string_view("scripts"),    std::string_view("session"),
                                                   std::string_view("snippets"),   std::string_view("system"),
                                                   std::string_view("time"),       std::string_view("timer"),
                                                   std::string_view("wallpaper"),  std::string_view("windowmanagement"),
                                                   std::string_view("windows")};
} // namespace launcher

struct LauncherCategory {
  std::string label;
  std::string glyphName;
};

struct LauncherResult {
  std::string id;
  std::string providerId; // Set by LauncherPanel after query; used for activation dispatch and usage tracking
  std::string title;
  // Optional source label for application-origin indicators and tooltips.
  std::string origin;
  std::string originGlyph;
  std::string subtitle;
  std::string glyphName;
  std::string iconName;
  std::string iconPath;
  // A short string drawn in the leading icon slot in place of an icon (e.g. an
  // emoji or a single symbol). When set, it replaces glyphName/iconName/iconPath.
  std::string badge;
  // AppProvider source desktop file path; empty for other providers.
  std::string desktopEntryPath;
  // When launching an application via AppProvider, matches DesktopAction::id (primary Exec leaves this empty).
  std::string desktopActionId;
  std::string category;
  std::string presentation;
  std::optional<std::string> query;
  double score = 0.0;
  // Set by LauncherPanel on the first result of each list section (Results, Files, Favourites, …);
  // the row draws it as a header above itself.
  std::string section;
  // Set by LauncherPanel: a short kind shown at the trailing edge (Application, File, …).
  std::string kind;
  // Set by LauncherPanel: the alias the user gave this result, drawn as a tag before the kind.
  std::string alias;
  // Fallback results (web search and friends) sort after everything else under their own header.
  bool fallback = false;
  int recentlyUsedIndex = 0; // Higher is more recent. <=0 means no record or too old.
  bool pinned = false;       // Set by LauncherPanel for launcher-owned pinned applications.
  // Set by LauncherPanel: a recently used result shown under Suggestions when nothing is typed.
  bool suggested = false;
};

// An extra action offered for a result in the actions menu (Shift+Return, or right click).
struct LauncherAction {
  std::string id;
  std::string label;
};

enum class LauncherActionOutcome {
  // Nothing happened; the launcher stays as it is.
  Failed,
  // Done; the launcher closes.
  Done,
  // Text was copied; the launcher closes and, for providers that support it, pastes.
  Pasted,
  // Done, and the launcher stays open with its results refreshed (pin, delete, …).
  KeepOpen,
};

// What the preview pane shows for the selected result, for providers that ask for one.
struct LauncherPreview {
  std::string title;
  std::string body;
  // A large emoji or symbol drawn above the title.
  std::string badge;
  // An image file, or encoded image bytes (PNG, JPEG, …) when there is no file.
  std::string imagePath;
  std::vector<std::uint8_t> imageBytes;
  // Label and value rows under the body (Type, Size, Modified, …).
  std::vector<std::pair<std::string, std::string>> metadata;
};

// A Raycast-style form shown in place of the results (Create Quicklink, Edit Snippet, …). The
// search field edits one field at a time; Return moves to the next field and finally submits.
struct LauncherFormField {
  std::string id;
  std::string label;
  std::string placeholder;
  std::string value;
  bool required = false;
  // Newlines are typed and shown as "\n" in the single-line search field.
  bool multiline = false;
};

struct LauncherForm {
  std::string title;
  std::string submitLabel;
  std::string glyph;
  std::vector<LauncherFormField> fields;
  // Saves the form. Returns an error to show under the submit row, or an empty string when saved.
  std::function<std::string(const std::vector<LauncherFormField>&)> submit;
};

class LauncherProvider {
public:
  virtual ~LauncherProvider() = default;

  [[nodiscard]] virtual std::string_view prefix() const {
    return m_customPrefix.has_value() ? *m_customPrefix : defaultPrefix();
  }
  [[nodiscard]] virtual std::string_view defaultPrefix() const = 0;

  virtual void setCustomPrefix(std::optional<std::string> prefix) { m_customPrefix = std::move(prefix); }
  [[nodiscard]] virtual bool allowCustomPrefix() const { return true; }

  // Stable opaque identity. Keys usage-tracking persistence and activation dispatch,
  // so it must never change or be translated.
  [[nodiscard]] virtual std::string_view id() const = 0;
  // Localizable title shown to the user (e.g. the prefix overview). Defaults to the
  // stable id(); override to return a translated string.
  [[nodiscard]] virtual std::string displayName() const { return std::string(id()); }
  [[nodiscard]] virtual std::string_view defaultGlyphName() const { return "search"; }

  // Return true to opt in to usage-based score boosting. The panel will
  // record each activation and surface frequently used entries higher.
  [[nodiscard]] virtual bool trackUsage() const { return false; }

  // Return true when activate() copies text and should honor shell.launcher.auto_paste
  // after the launcher closes (calculator, emoji, copy-mode dmenu, …).
  [[nodiscard]] virtual bool supportsAutoPaste() const { return false; }

  // Prefixed providers (non-empty prefix()) normally only respond when their prefix is typed.
  // Return true to also contribute results to the general (non-prefixed) search.
  [[nodiscard]] virtual bool includeInGlobalSearch() const {
    return m_customGlobalSearch.has_value() ? *m_customGlobalSearch : defaultIncludeInGlobalSearch();
  }
  [[nodiscard]] virtual bool defaultIncludeInGlobalSearch() const { return false; }
  virtual void setCustomIncludeInGlobalSearch(std::optional<bool> value) { m_customGlobalSearch = value; }

  [[nodiscard]] virtual std::vector<LauncherCategory> categories() const { return {}; }

  // True for providers registered dynamically (plugin-backed). The panel can drop
  // and re-add just these when the enabled plugin set changes.
  [[nodiscard]] virtual bool isDynamic() const { return false; }

  // Async providers (plugin-backed) deliver results after query() returns; the
  // panel installs this callback so the provider can ask for the current query to
  // be re-gathered when fresh results land. Synchronous providers ignore it.
  virtual void setResultsChangedCallback(std::function<void()> /*callback*/) {}

  // True while an async provider is still producing results. The launcher uses
  // it to show a loading state instead of "No results found".
  [[nodiscard]] virtual bool isLoading() const { return false; }

  // Plugin-backed providers can request that the open launcher input be replaced,
  // e.g. to implement autocomplete.
  virtual void setQueryRequestedCallback(std::function<void(std::string)> /*callback*/) {}

  // Providers with create/edit commands ask the launcher to show a form through this.
  virtual void setFormRequestedCallback(std::function<void(LauncherForm)> /*callback*/) {}

  // Async (plugin-backed) providers defer the launcher close until their activation
  // handler resolves: if it rewrote the query the panel stays open, otherwise the
  // provider invokes this to close it (and record usage). Arguments are the
  // activated result id and whether onActivate copied to the clipboard.
  virtual void setActivationDoneCallback(std::function<void(std::string, bool)> /*callback*/) {}

  virtual void initialize() {}

  // Called when the launcher panel closes. Async providers should drop any cached
  // result set so a later session doesn't briefly show the previous query's results.
  virtual void reset() {}

  [[nodiscard]] virtual std::vector<LauncherResult> query(std::string_view text) const = 0;
  // Query after this provider's prefix was explicitly matched.
  [[nodiscard]] virtual std::vector<LauncherResult> queryPrefixed(std::string_view text) const { return query(text); }

  virtual bool activate(const LauncherResult& result) = 0;

  // Text that Tab puts in the search field for this result (a quicklink's keyword, a provider's
  // prefix), like Raycast's autocomplete. Empty when there is nothing to complete.
  [[nodiscard]] virtual std::string completion(const LauncherResult& /*result*/) const { return {}; }

  // The primary action's name, shown in the action bar and at the top of the actions menu.
  // Empty means the launcher's generic "Open".
  [[nodiscard]] virtual std::string primaryActionLabel(const LauncherResult& /*result*/) const { return {}; }
  // Extra actions for a result, listed under the primary action.
  [[nodiscard]] virtual std::vector<LauncherAction> actions(const LauncherResult& /*result*/) const { return {}; }
  virtual LauncherActionOutcome runAction(const LauncherResult& /*result*/, std::string_view /*actionId*/) {
    return LauncherActionOutcome::Failed;
  }

  // Providers whose results read better beside a preview (clipboard, snippets, files) return true;
  // the launcher then splits into a list and a preview pane while only this provider is shown.
  [[nodiscard]] virtual bool showsPreview() const { return false; }
  [[nodiscard]] virtual std::optional<LauncherPreview> preview(const LauncherResult& /*result*/) const {
    return std::nullopt;
  }

  // True when result ids are stable, so a result can carry an alias and be run by id
  // (`noctalia msg launcher-run`) without the launcher open.
  [[nodiscard]] virtual bool supportsAliases() const { return false; }
  // Finds a result by id. The default searches the unfiltered listing.
  [[nodiscard]] virtual std::optional<LauncherResult> resultForId(std::string_view resultId) const {
    for (auto& result : queryPrefixed({})) {
      if (result.id == resultId) {
        return result;
      }
    }
    for (auto& result : query({})) {
      if (result.id == resultId) {
        return result;
      }
    }
    return std::nullopt;
  }

private:
  std::optional<std::string> m_customPrefix;
  std::optional<bool> m_customGlobalSearch;
};
