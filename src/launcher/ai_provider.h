#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"
#include "net/http_client.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

// Raycast's AI in the launcher: Quick AI from the root search ("Ask AI" under the fallbacks, or
// /ai), the answer streaming into the launcher as Markdown, and AI Commands (Improve Writing, Fix
// Spelling, Summarize, Translate, …) that run a prompt over the clipboard text. Return copies the
// answer (and pastes it, per auto_paste).
//
// The model comes from the service in shell.launcher.ai.provider: a local Ollama (the default,
// nothing leaves the machine), an OpenAI-compatible API (OpenAI, OpenRouter, Groq, Mistral, LM
// Studio, …) or Anthropic. Online services need an API key from `api_key`, `api_key_command` or
// the service's usual environment variable. Models come from the service's list endpoint; the
// pick is kept per service in the state directory.
class AiProvider : public LauncherProvider {
public:
  using Kind = AiProviderKind;

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

  // The service's base URL when `url` is left empty, and its default model when nothing is listed.
  [[nodiscard]] static std::string defaultUrl(Kind kind);
  [[nodiscard]] static std::string defaultModel(Kind kind);
  // The environment variable the service's own tools read the key from.
  [[nodiscard]] static std::string_view keyEnvironmentVariable(Kind kind);
  // The requests for each service; exposed for tests.
  [[nodiscard]] static HttpRequest modelsRequest(Kind kind, std::string_view baseUrl, std::string_view apiKey);
  [[nodiscard]] static HttpRequest chatRequest(
      Kind kind, std::string_view baseUrl, std::string_view apiKey, std::string_view model, std::string_view prompt
  );
  struct Message {
    std::string role;
    std::string content;
  };
  [[nodiscard]] static HttpRequest chatRequest(
      Kind kind, std::string_view baseUrl, std::string_view apiKey, std::string_view model,
      const std::vector<Message>& conversation
  );

  // An independent, in-memory conversation for the Island. Opening the panel never submits a request.
  bool submitQuestion(std::string question);
  void stopConversation();
  void clearConversation();
  void retryConversation();
  [[nodiscard]] std::string_view question() const;
  [[nodiscard]] std::string_view answer() const;
  [[nodiscard]] std::string_view error() const;
  [[nodiscard]] bool streaming() const;
  [[nodiscard]] bool interrupted() const;
  enum class GenerationStage { Thinking, LoadingModel, Waiting };
  [[nodiscard]] GenerationStage generationStage() const;
  struct LocalStatus {
    bool loaded = true;
    bool busy = false;
  };
  [[nodiscard]] static std::string localStatusUrl(Kind kind, std::string_view endpoint);
  [[nodiscard]] static std::optional<LocalStatus> parseLocalStatus(std::string_view body);
  // Model names from the service's list response; exposed for tests.
  [[nodiscard]] static std::vector<std::string> parseModels(Kind kind, std::string_view json);
  // One line of the service's stream (NDJSON for Ollama, SSE "data:" lines for the others), and
  // whether it was the last; nullopt for lines that carry nothing (events, blanks). Exposed for tests.
  struct StreamLine {
    std::string content;
    std::string error;
    bool done = false;
    bool processing = false;
  };
  [[nodiscard]] static std::optional<StreamLine> parseStreamLine(Kind kind, std::string_view line);

  [[nodiscard]] Kind kind() const;
  // The service's base URL from config, without a trailing slash.
  [[nodiscard]] std::string baseUrl() const;
  // The model that answers: the config one, else the saved pick, else the first listed, else the default.
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
    std::vector<Message> messages;
    std::string endpoint;
    Kind kind = Kind::Ollama;
    bool interrupted = false;
    bool receivedDone = false;
    bool processing = false;
    bool waiting = false;
    GenerationStage stage = GenerationStage::Thinking;
  };

  [[nodiscard]] std::string apiKey() const;
  [[nodiscard]] bool needsKey() const;
  void refreshModels(bool force) const;
  void ask(std::string view, std::string heading, std::string prompt, std::vector<Message> messages = {});
  void stopStream();
  void startSessionStream();
  void probeLocalStatus(bool beforeSubmit);
  void stopStatusProbe();
  void savePick(const std::string& name);
  void loadPicks() const;
  [[nodiscard]] std::string clipboardInput() const;
  [[nodiscard]] std::string serviceName() const;
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
  mutable Kind m_modelsKind = Kind::Ollama;
  mutable std::string m_modelsUrl;
  mutable bool m_modelsLoading = false;
  mutable bool m_modelsFailed = false;
  mutable long m_modelsStatus = 0;
  mutable std::chrono::steady_clock::time_point m_modelsFetched{};
  mutable std::optional<std::map<std::string, std::string>> m_picks; // service name → model
  mutable std::optional<std::string> m_commandKey;                   // api_key_command's output, once
  mutable std::string m_commandKeySource;
  std::optional<Session> m_session;
  std::uint64_t m_requestGeneration = 0;
  HttpClient::StreamId m_statusStream = 0;
  Timer m_statusDeadline;
  Timer m_statusRefresh;
  std::shared_ptr<void> m_alive = std::make_shared<int>(0);
};
