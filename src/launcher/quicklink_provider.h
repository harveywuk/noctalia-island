#pragma once

#include "config/config_types.h"
#include "launcher/launcher_provider.h"

#include <vector>

class ClipboardService;
class ConfigService;

// Raycast-style quicklinks: open a saved URL by name, or type its keyword and a query
// ("gh noctalia") to search with it. Also offers "Search the web" as a fallback for any query.
class QuicklinkProvider : public LauncherProvider {
public:
  QuicklinkProvider(ConfigService* config, ClipboardService* clipboard);

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

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherResult> resultForId(std::string_view resultId) const override;

  // The configured quicklinks, or the built-in set when none are configured.
  [[nodiscard]] static std::vector<LauncherQuicklinkConfig> effectiveQuicklinks(const ConfigService* config);
  // Results for `text` against `links`; exposed for tests.
  [[nodiscard]] static std::vector<LauncherResult>
  match(const std::vector<LauncherQuicklinkConfig>& links, std::string_view text, bool listAll);

private:
  [[nodiscard]] std::string urlFor(const LauncherResult& result) const;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::function<void(std::string)> m_requestQuery;
};
