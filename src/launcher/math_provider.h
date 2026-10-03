#pragma once

#include "launcher/launcher_provider.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>

class ClipboardService;
class ConfigService;
class HttpClient;
class Calculator;

class MathProvider : public LauncherProvider {
public:
  MathProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient);
  ~MathProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "calc"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Calculator"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "calculator"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  void initialize() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

  struct HistoryEntry {
    std::string expression;
    std::string result;
  };
  // Raycast keeps the last answers under the calculator; exposed for tests through `path`.
  [[nodiscard]] static std::deque<HistoryEntry> loadHistory(const std::string& path);
  static void saveHistory(const std::string& path, const std::deque<HistoryEntry>& history);

private:
  [[nodiscard]] std::vector<LauncherResult> evaluate(std::string_view text) const;
  [[nodiscard]] std::vector<LauncherResult> historyResults() const;
  void remember(std::string expression, std::string result);
  [[nodiscard]] std::string historyPath() const;

  // Download fresh exchange rates over the async HTTP client, gated on
  // shell.launcher.fetch_exchange_rates and shell.offline_mode.
  void refreshExchangeRates();

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  HttpClient* m_httpClient = nullptr;
  std::unique_ptr<Calculator> m_calc;
  mutable std::optional<std::deque<HistoryEntry>> m_history;
};
