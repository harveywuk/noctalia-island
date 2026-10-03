#pragma once

#include "launcher/launcher_provider.h"
#include "net/http_client.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

// Raycast's AI, served by a local Ollama (shell.launcher.ai.url): Quick AI from the root search
// ("Ask AI" under the fallbacks, or /ai), the answer streaming into the launcher as Markdown, and
// AI Commands (Improve Writing, Fix Spelling, Summarize, Translate, …) that run a prompt over the
// clipboard text. Return copies the answer (and pastes it, per auto_paste). Models come from
// /api/tags; the pick is kept in the state directory. Nothing leaves the machine unless the URL
// points elsewhere.
class AiProvider : public LauncherProvider {
public:
  struct Command {
    std::string id;
    std::string titleKey;
    std::string prompt; // {text} is the input, {language} a Translate argument
    std::string glyph;
    bool takesLanguage = false;
  };

  AiProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient);
  ~AiProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "ai"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "AI"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "sparkles"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }
  [[nodiscard]] bool isLoading() const override;
  void setResultsChangedCallback(std::function<void()> callback) override { m_onChanged = std::move(callback); }
  void setQueryRequestedCallback(std::function<void(std::string)> callback) override {
    m_requestQuery = std::move(callback);
  }
  void reset() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherResult> resultForId(std::string_view resultId) const override;

  // The built-in AI commands; exposed for tests.
  [[nodiscard]] static const std::vector<Command>& commands();
  // Fills a command's prompt; exposed for tests.
  [[nodiscard]] static std::string
  fillPrompt(std::string_view prompt, std::string_view text, std::string_view language = {});
  // Model names from an /api/tags response; exposed for tests.
  [[nodiscard]] static std::vector<std::string> parseModels(std::string_view json);
  // The text of one /api/chat stream line, and whether it was the last; exposed for tests.
  struct StreamLine {
    std::string content;
    std::string error;
    bool done = false;
  };
  [[nodiscard]] static std::optional<StreamLine> parseStreamLine(std::string_view line);
  // The Ollama base URL from config, without a trailing slash.
  [[nodiscard]] std::string baseUrl() const;
  // The model that answers: the config one, else the saved pick, else the first installed.
  [[nodiscard]] std::string model() const;

private:
  struct Session {
    std::string view;    // the query text the answer shows under
    std::string heading; // what was asked, for the answer's subtitle
    std::string prompt;
    std::string model;
    std::string answer;
    std::string error;
    std::string lineBuffer;
    bool streaming = false;
    HttpClient::StreamId stream = 0;
  };

  void refreshModels(bool force) const;
  void ask(std::string view, std::string heading, std::string prompt);
  void stopStream();
  void savePick(const std::string& name);
  void loadPick() const;
  [[nodiscard]] std::string clipboardInput() const;
  [[nodiscard]] LauncherResult commandRow(const Command& command, double score) const;
  [[nodiscard]] LauncherResult askRow(std::string_view question, double score, bool fallback) const;
  [[nodiscard]] std::vector<LauncherResult> answerRows() const;
  [[nodiscard]] std::vector<LauncherResult> modelRows(std::string_view filter) const;
  [[nodiscard]] std::optional<LauncherResult> statusRow() const;

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  HttpClient* m_httpClient = nullptr;
  std::function<void()> m_onChanged;
  std::function<void(std::string)> m_requestQuery;
  mutable std::vector<std::string> m_models;
  mutable bool m_modelsLoading = false;
  mutable bool m_modelsFailed = false;
  mutable std::chrono::steady_clock::time_point m_modelsFetched{};
  mutable std::optional<std::string> m_pick;
  std::optional<Session> m_session;
};
