#pragma once

#include "config/config_types.h"
#include "launcher/launcher_provider.h"

#include <vector>

class ClipboardService;
class ConfigService;
class QuicklinkStore;

// Raycast-style quicklinks: open a saved URL by name, or type its keyword and a query
// ("gh noctalia") to search with it. Also offers "Search the web" as a fallback for any query.
class QuicklinkProvider : public LauncherProvider {
public:
  QuicklinkProvider(ConfigService* config, ClipboardService* clipboard, QuicklinkStore* store = nullptr);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "link"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Quicklinks"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "link"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }

  void setQueryRequestedCallback(std::function<void(std::string)> callback) override {
    m_requestQuery = std::move(callback);
  }
  void setFormRequestedCallback(std::function<void(LauncherForm)> callback) override {
    m_requestForm = std::move(callback);
  }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::string completion(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherResult> resultForId(std::string_view resultId) const override;

  [[nodiscard]] static std::vector<LauncherQuicklinkConfig> builtinQuicklinks();
  // The built-in quicklinks, with config and saved ones replacing or adding to them by id.
  [[nodiscard]] static std::vector<LauncherQuicklinkConfig>
  effectiveQuicklinks(const ConfigService* config, QuicklinkStore* store = nullptr);
  // "github.com" → "https://github.com"; empty when it can't be a link. Exposed for tests.
  [[nodiscard]] static std::string normalizeUrl(std::string_view input);
  // The form's error for these values, or empty; `id` is the link being edited. Exposed for tests.
  [[nodiscard]] static std::string validate(
      const std::vector<LauncherQuicklinkConfig>& links, std::string_view id, std::string_view url,
      std::string_view keyword
  );
  // Results for `text` against `links`; exposed for tests.
  [[nodiscard]] static std::vector<LauncherResult>
  match(const std::vector<LauncherQuicklinkConfig>& links, std::string_view text, bool listAll);
  // Fallback rows ("Search GitHub for …") for the search quicklinks named in `fallbackIds`; exposed for tests.
  [[nodiscard]] static std::vector<LauncherResult> fallbacks(
      const std::vector<LauncherQuicklinkConfig>& links, const std::vector<std::string>& fallbackIds,
      std::string_view query
  );

private:
  [[nodiscard]] std::string urlFor(const LauncherResult& result) const;
  [[nodiscard]] std::vector<LauncherQuicklinkConfig> links() const;
  [[nodiscard]] std::optional<LauncherQuicklinkConfig> linkFor(std::string_view resultId) const;
  [[nodiscard]] bool isSaved(std::string_view id) const;
  [[nodiscard]] bool isEditable(std::string_view id) const;
  [[nodiscard]] LauncherForm form(const LauncherQuicklinkConfig* existing) const;
  [[nodiscard]] std::optional<LauncherResult> createCommand(std::string_view text, bool listAll) const;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  QuicklinkStore* m_store = nullptr;
  std::function<void(std::string)> m_requestQuery;
  std::function<void(LauncherForm)> m_requestForm;
};
