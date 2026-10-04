#include "launcher/ai_provider.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

  constexpr std::string_view kAskId = "ask";
  constexpr std::string_view kAnswerId = "answer";
  constexpr std::string_view kModelsId = "models";
  constexpr std::string_view kStatusId = "status";
  constexpr std::string_view kModelPrefix = "model:";
  constexpr std::string_view kCommandPrefix = "command:";
  constexpr std::string_view kModelsKeyword = "model";
  constexpr std::string_view kPickFile = "ai_model.json";
  constexpr std::string_view kAnthropicVersion = "2023-06-01";
  constexpr int kAnthropicMaxTokens = 2048;
  constexpr double kAskScore = 9500.0;
  constexpr double kCommandScore = 9000.0;
  constexpr double kFallbackScore = -10000.0;
  constexpr auto kModelsTtl = std::chrono::seconds(30);
  constexpr std::size_t kMaxInputChars = 12000;

  constexpr std::string_view kRewriteRules =
      "Reply with only the rewritten text: no preamble, no quotes around it, no explanation. Keep the "
      "original language, meaning, formatting, URLs and emojis.";

  [[nodiscard]] std::string trimSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
      url.pop_back();
    }
    return url;
  }

  [[nodiscard]] std::string_view kindName(AiProviderKind kind) {
    for (const auto& option : kAiProviderKinds) {
      if (option.value == kind) {
        return option.key;
      }
    }
    return "ollama";
  }

  // The message of an error body ({"error": "…"} or {"error": {"message": "…"}}), or empty.
  [[nodiscard]] std::string errorMessage(const nlohmann::json& parsed) {
    if (!parsed.is_object() || !parsed.contains("error")) {
      return {};
    }
    const auto& error = parsed["error"];
    if (error.is_string()) {
      return error.get<std::string>();
    }
    if (error.is_object()) {
      const std::string message = error.value("message", "");
      return message.empty() ? error.dump() : message;
    }
    return error.dump();
  }

} // namespace

const std::vector<AiProvider::Command>& AiProvider::commands() {
  static const std::vector<Command> kCommands = {
      {.id = "improve",
       .titleKey = "launcher.ai.commands.improve",
       .prompt = std::string(
                     "Act as a writing assistant. Improve the text: fix spelling, grammar and punctuation, "
                     "make it clearer and more concise, prefer the active voice, and keep its tone. "
                 )
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "wand"},
      {.id = "fix",
       .titleKey = "launcher.ai.commands.fix",
       .prompt = std::string(
                     "Act as a spelling and grammar corrector. Correct spelling, grammar and punctuation and "
                     "change nothing else. "
                 )
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "text-grammar"},
      {.id = "shorter",
       .titleKey = "launcher.ai.commands.shorter",
       .prompt = std::string("Make the text shorter while keeping every key point. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "arrows-minimize"},
      {.id = "longer",
       .titleKey = "launcher.ai.commands.longer",
       .prompt = std::string("Make the text longer with more detail, without padding or repetition. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "arrows-maximize"},
      {.id = "professional",
       .titleKey = "launcher.ai.commands.professional",
       .prompt = std::string("Rewrite the text in a professional, formal tone. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "briefcase"},
      {.id = "casual",
       .titleKey = "launcher.ai.commands.casual",
       .prompt = std::string("Rewrite the text in a casual, relaxed tone. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "mood-smile"},
      {.id = "friendly",
       .titleKey = "launcher.ai.commands.friendly",
       .prompt = std::string("Rewrite the text in a warm, friendly tone. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "heart"},
      {.id = "confident",
       .titleKey = "launcher.ai.commands.confident",
       .prompt = std::string("Rewrite the text in a confident, direct tone, dropping hedges and filler. ")
           + std::string(kRewriteRules)
           + "\n\nText:\n{text}",
       .glyph = "bolt"},
      {.id = "summarize",
       .titleKey = "launcher.ai.commands.summarize",
       .prompt = "Summarize the text in a few short bullet points that capture its key points. Reply with only the "
                 "bullet points, in the text's own language.\n\nText:\n{text}",
       .glyph = "list-details"},
      {.id = "explain",
       .titleKey = "launcher.ai.commands.explain",
       .prompt = "Explain the text in simple, concise language. For a single word give a short definition; for a "
                 "passage break the main ideas into plain terms, with an example or analogy when it helps. Reply "
                 "with only the explanation.\n\nText:\n{text}",
       .glyph = "bulb"},
      {.id = "code",
       .titleKey = "launcher.ai.commands.code",
       .prompt = "Explain this code step by step: what it does, how each part works and anything surprising. Use "
                 "short paragraphs or a numbered list, in Markdown.\n\nCode:\n{text}",
       .glyph = "code"},
      {.id = "translate",
       .titleKey = "launcher.ai.commands.translate",
       .prompt = std::string("Translate the text into {language}. ") + std::string(kRewriteRules) + "\n\nText:\n{text}",
       .glyph = "language",
       .takesLanguage = true},
  };
  return kCommands;
}

std::string AiProvider::fillPrompt(std::string_view prompt, std::string_view text, std::string_view language) {
  std::string out(prompt);
  const auto replaceAll = [&out](std::string_view token, std::string_view value) {
    for (std::size_t pos = out.find(token); pos != std::string::npos; pos = out.find(token, pos + value.size())) {
      out.replace(pos, token.size(), value);
    }
  };
  replaceAll("{language}", language);
  replaceAll("{text}", text);
  return out;
}

std::string AiProvider::defaultUrl(Kind kind) {
  switch (kind) {
  case Kind::OpenAi:
    return "https://api.openai.com/v1";
  case Kind::Anthropic:
    return "https://api.anthropic.com/v1";
  case Kind::Ollama:
    break;
  }
  return "http://127.0.0.1:11434";
}

std::string AiProvider::defaultModel(Kind kind) {
  switch (kind) {
  case Kind::OpenAi:
    return "gpt-4o-mini";
  case Kind::Anthropic:
    return "claude-sonnet-5-5";
  case Kind::Ollama:
    break;
  }
  return {};
}

std::string_view AiProvider::keyEnvironmentVariable(Kind kind) {
  switch (kind) {
  case Kind::OpenAi:
    return "OPENAI_API_KEY";
  case Kind::Anthropic:
    return "ANTHROPIC_API_KEY";
  case Kind::Ollama:
    break;
  }
  return "OLLAMA_API_KEY";
}

HttpRequest AiProvider::modelsRequest(Kind kind, std::string_view baseUrl, std::string_view apiKey) {
  HttpRequest request;
  request.followRedirects = true;
  const std::string base = trimSlash(std::string(baseUrl));
  switch (kind) {
  case Kind::OpenAi:
    request.url = base + "/models";
    request.headers = {"Authorization: Bearer " + std::string(apiKey)};
    break;
  case Kind::Anthropic:
    request.url = base + "/models";
    request.headers = {"x-api-key: " + std::string(apiKey), "anthropic-version: " + std::string(kAnthropicVersion)};
    break;
  case Kind::Ollama:
    request.url = base + "/api/tags";
    if (!apiKey.empty()) {
      request.headers = {"Authorization: Bearer " + std::string(apiKey)};
    }
    break;
  }
  return request;
}

HttpRequest AiProvider::chatRequest(
    Kind kind, std::string_view baseUrl, std::string_view apiKey, std::string_view model, std::string_view prompt
) {
  HttpRequest request;
  request.method = "POST";
  const std::string base = trimSlash(std::string(baseUrl));
  const nlohmann::json messages = nlohmann::json::array({{{"role", "user"}, {"content", std::string(prompt)}}});
  switch (kind) {
  case Kind::OpenAi:
    request.url = base + "/chat/completions";
    request.headers = {"Content-Type: application/json", "Authorization: Bearer " + std::string(apiKey)};
    request.body = nlohmann::json{{"model", std::string(model)}, {"stream", true}, {"messages", messages}}.dump();
    break;
  case Kind::Anthropic:
    request.url = base + "/messages";
    request.headers = {
        "content-type: application/json", "x-api-key: " + std::string(apiKey),
        "anthropic-version: " + std::string(kAnthropicVersion)
    };
    request.body =
        nlohmann::json{
            {"model", std::string(model)},
            {"max_tokens", kAnthropicMaxTokens},
            {"stream", true},
            {"messages", messages},
        }
            .dump();
    break;
  case Kind::Ollama:
    request.url = base + "/api/chat";
    request.headers = {"Content-Type: application/json"};
    if (!apiKey.empty()) {
      request.headers.push_back("Authorization: Bearer " + std::string(apiKey));
    }
    request.body = nlohmann::json{{"model", std::string(model)}, {"stream", true}, {"messages", messages}}.dump();
    break;
  }
  return request;
}

std::vector<std::string> AiProvider::parseModels(Kind kind, std::string_view json) {
  std::vector<std::string> names;
  try {
    const auto parsed = nlohmann::json::parse(json);
    const char* listKey = kind == Kind::Ollama ? "models" : "data";
    const char* nameKey = kind == Kind::Ollama ? "name" : "id";
    if (!parsed.contains(listKey) || !parsed[listKey].is_array()) {
      return names;
    }
    for (const auto& model : parsed[listKey]) {
      const std::string name = model.value(nameKey, "");
      if (!name.empty()) {
        names.push_back(name);
      }
    }
  } catch (const nlohmann::json::exception&) {
  }
  if (kind != Kind::Ollama) {
    std::ranges::sort(names);
  }
  return names;
}

std::optional<AiProvider::StreamLine> AiProvider::parseStreamLine(Kind kind, std::string_view line) {
  std::string trimmed = StringUtils::trim(line);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (kind != Kind::Ollama) {
    // Server-sent events: "event:" lines name the next "data:" line; anything else that isn't JSON
    // (comments, ids) carries nothing. A bare JSON object is an error body outside the stream.
    if (trimmed.starts_with("data:")) {
      trimmed = StringUtils::trim(std::string_view(trimmed).substr(5));
      if (trimmed == "[DONE]") {
        return StreamLine{.done = true};
      }
    } else if (!trimmed.starts_with('{')) {
      return std::nullopt;
    }
  }
  try {
    const auto parsed = nlohmann::json::parse(trimmed);
    StreamLine out;
    if (const std::string error = errorMessage(parsed); !error.empty()) {
      out.error = error;
      out.done = true;
      return out;
    }
    switch (kind) {
    case Kind::OpenAi:
      if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
        const auto& choice = parsed["choices"][0];
        if (choice.contains("delta")
            && choice["delta"].is_object()
            && choice["delta"].contains("content")
            && choice["delta"]["content"].is_string()) {
          out.content = choice["delta"]["content"].get<std::string>();
        }
        out.done = choice.contains("finish_reason") && !choice["finish_reason"].is_null();
      }
      return out;
    case Kind::Anthropic: {
      const std::string type = parsed.value("type", "");
      if (type == "content_block_delta" && parsed.contains("delta") && parsed["delta"].is_object()) {
        out.content = parsed["delta"].value("text", "");
      } else if (type == "message_stop") {
        out.done = true;
      } else if (type == "error") {
        out.error = errorMessage(parsed).empty() ? parsed.dump() : errorMessage(parsed);
        out.done = true;
      }
      return out;
    }
    case Kind::Ollama:
      if (parsed.contains("message") && parsed["message"].is_object()) {
        out.content = parsed["message"].value("content", "");
      } else if (parsed.contains("response")) {
        out.content = parsed.value("response", ""); // /api/generate shape
      }
      out.done = parsed.value("done", false);
      return out;
    }
    return out;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

AiProvider::AiProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient)
    : m_clipboard(clipboard), m_config(config), m_httpClient(httpClient) {}

AiProvider::~AiProvider() { stopStream(); }

std::string AiProvider::displayName() const { return i18n::tr("launcher.providers.ai.title"); }

AiProvider::Kind AiProvider::kind() const {
  return m_config != nullptr ? m_config->config().shell.launcher.ai.provider : Kind::Ollama;
}

std::string AiProvider::serviceName() const {
  return i18n::tr("settings.options.launcher.ai-provider." + std::string(kindName(kind())));
}

std::string AiProvider::baseUrl() const {
  std::string url = m_config != nullptr ? m_config->config().shell.launcher.ai.url : std::string();
  if (url.empty()) {
    url = defaultUrl(kind());
  }
  return trimSlash(std::move(url));
}

bool AiProvider::needsKey() const { return kind() != Kind::Ollama; }

std::string AiProvider::apiKey() const {
  if (m_config == nullptr) {
    return {};
  }
  const auto& ai = m_config->config().shell.launcher.ai;
  if (!ai.apiKey.empty()) {
    return ai.apiKey;
  }
  if (!ai.apiKeyCommand.empty()) {
    // Run once per command text; a password manager may prompt, so the result is kept.
    if (!m_commandKey.has_value() || m_commandKeySource != ai.apiKeyCommand) {
      const process::RunResult result = process::runSync(ai.apiKeyCommand);
      m_commandKey = result ? StringUtils::trim(result.out) : std::string();
      m_commandKeySource = ai.apiKeyCommand;
    }
    if (!m_commandKey->empty()) {
      return *m_commandKey;
    }
  }
  const char* env = std::getenv(std::string(keyEnvironmentVariable(kind())).c_str());
  return env != nullptr ? std::string(env) : std::string();
}

void AiProvider::loadPicks() const {
  if (m_picks.has_value()) {
    return;
  }
  m_picks.emplace();
  std::ifstream in(std::filesystem::path(FileUtils::stateDir()) / kPickFile);
  if (!in.is_open()) {
    return;
  }
  try {
    const auto parsed = nlohmann::json::parse(in);
    if (parsed.is_object()) {
      for (const auto& [key, value] : parsed.items()) {
        if (value.is_string()) {
          if (key == "model") {
            (*m_picks)["ollama"] = value.get<std::string>(); // the first version kept one pick
          } else {
            (*m_picks)[key] = value.get<std::string>();
          }
        }
      }
    }
  } catch (const nlohmann::json::exception&) {
  }
}

void AiProvider::savePick(const std::string& name) {
  loadPicks();
  (*m_picks)[std::string(kindName(kind()))] = name;
  const std::filesystem::path dir(FileUtils::stateDir());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir / kPickFile, std::ios::trunc);
  out << nlohmann::json(*m_picks).dump(2) << '\n';
}

std::string AiProvider::model() const {
  if (m_config != nullptr && !m_config->config().shell.launcher.ai.model.empty()) {
    return m_config->config().shell.launcher.ai.model;
  }
  loadPicks();
  const auto pick = m_picks->find(std::string(kindName(kind())));
  if (pick != m_picks->end()
      && !pick->second.empty()
      && (m_models.empty() || std::ranges::find(m_models, pick->second) != m_models.end())) {
    return pick->second;
  }
  if (!m_models.empty()) {
    return m_models.front();
  }
  return defaultModel(kind());
}

bool AiProvider::isLoading() const { return m_modelsLoading || (m_session.has_value() && m_session->streaming); }

void AiProvider::reset() {
  // Keep the answer: coming back to the same question shows it again. Only the model list ages out.
}

void AiProvider::refreshModels(bool force) const {
  if (m_httpClient == nullptr || m_modelsLoading) {
    return;
  }
  const Kind current = kind();
  const std::string url = baseUrl();
  if (current != m_modelsKind || url != m_modelsUrl) {
    force = true; // the service changed under us
    m_models.clear();
    m_modelsFailed = false;
    m_modelsStatus = 0;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!force && m_modelsFetched != std::chrono::steady_clock::time_point{} && now - m_modelsFetched < kModelsTtl) {
    return;
  }
  const std::string key = apiKey();
  if (needsKey() && key.empty()) {
    return; // the status row says what to set
  }
  m_modelsLoading = true;
  m_modelsKind = current;
  m_modelsUrl = url;
  m_httpClient->request(modelsRequest(current, url, key), [this, current](HttpResponse response) {
    m_modelsLoading = false;
    m_modelsFetched = std::chrono::steady_clock::now();
    m_modelsStatus = response.status;
    m_modelsFailed = !(response.transportOk && response.status == 200);
    if (!m_modelsFailed) {
      m_models = parseModels(current, response.body);
    }
    if (m_onChanged) {
      m_onChanged();
    }
  });
}

std::string AiProvider::clipboardInput() const {
  if (m_clipboard == nullptr) {
    return {};
  }
  std::string text = StringUtils::trim(m_clipboard->clipboardText().value_or(std::string()));
  if (text.size() > kMaxInputChars) {
    text = StringUtils::truncateUtf8(text, kMaxInputChars);
  }
  return text;
}

LauncherResult AiProvider::askRow(std::string_view question, double score, bool fallback) const {
  LauncherResult row;
  row.id = std::string(kAskId);
  row.query = std::string(question);
  row.title = i18n::tr("launcher.ai.ask", "question", std::string(question));
  const std::string current = model();
  row.subtitle = current.empty() ? i18n::tr("launcher.ai.no-model") : current + " · " + serviceName();
  row.glyphName = "sparkles";
  row.kind = i18n::tr("launcher.kinds.ai");
  row.score = score;
  row.fallback = fallback;
  return row;
}

LauncherResult AiProvider::commandRow(const Command& command, double score) const {
  LauncherResult row;
  row.id = std::string(kCommandPrefix) + command.id;
  row.title = i18n::tr(command.titleKey);
  const std::string input = clipboardInput();
  row.subtitle = input.empty() ? i18n::tr("launcher.ai.copy-text-first")
                               : i18n::tr("launcher.ai.on-clipboard", "text", StringUtils::truncateUtf8(input, 60));
  row.glyphName = command.glyph;
  row.kind = i18n::tr("launcher.kinds.ai-command");
  row.score = score;
  if (command.takesLanguage) {
    row.arguments.push_back({.id = "language", .placeholder = i18n::tr("launcher.ai.language"), .required = true});
  }
  return row;
}

std::optional<LauncherResult> AiProvider::statusRow() const {
  LauncherResult row;
  row.id = std::string(kStatusId);
  row.glyphName = "plug-connected-x";
  row.kind = i18n::tr("launcher.kinds.ai");
  row.score = kAskScore + 1.0;
  if (m_config != nullptr && m_config->config().shell.offlineMode) {
    row.title = i18n::tr("launcher.ai.offline");
    return row;
  }
  if (needsKey() && apiKey().empty()) {
    row.glyphName = "key";
    row.title = i18n::tr("launcher.ai.no-key", "service", serviceName());
    row.subtitle = i18n::tr("launcher.ai.no-key-subtitle", "variable", std::string(keyEnvironmentVariable(kind())));
    return row;
  }
  if (m_modelsFailed) {
    if (needsKey() && (m_modelsStatus == 401 || m_modelsStatus == 403)) {
      row.glyphName = "key-off";
      row.title = i18n::tr("launcher.ai.key-rejected", "service", serviceName());
      row.subtitle = i18n::tr("launcher.ai.key-rejected-subtitle");
      return row;
    }
    row.title = i18n::tr("launcher.ai.unreachable", "service", serviceName(), "url", baseUrl());
    row.subtitle = kind() == Kind::Ollama ? i18n::tr("launcher.ai.unreachable-subtitle")
                                          : i18n::tr("launcher.ai.unreachable-online-subtitle");
    return row;
  }
  if (!m_modelsLoading
      && m_modelsFetched != std::chrono::steady_clock::time_point{}
      && m_models.empty()
      && kind() == Kind::Ollama) {
    row.title = i18n::tr("launcher.ai.no-models");
    row.subtitle = i18n::tr("launcher.ai.no-models-subtitle");
    return row;
  }
  return std::nullopt;
}

std::vector<LauncherResult> AiProvider::answerRows() const {
  LauncherResult row;
  row.id = std::string(kAnswerId);
  row.presentation = "markdown";
  row.glyphName = "sparkles";
  row.kind = i18n::tr("launcher.kinds.ai");
  if (!m_session->error.empty()) {
    row.title = i18n::tr("launcher.ai.failed", "error", m_session->error);
    row.subtitle = m_session->heading;
  } else if (m_session->answer.empty()) {
    row.title = i18n::tr("launcher.ai.thinking");
    row.subtitle = m_session->heading + " · " + m_session->model;
  } else {
    row.title = m_session->answer;
    row.subtitle = m_session->heading
        + " · "
        + m_session->model
        + (m_session->streaming ? " · " + i18n::tr("launcher.ai.writing") : std::string());
  }
  return {std::move(row)};
}

std::vector<LauncherResult> AiProvider::modelRows(std::string_view filter) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(filter));
  const std::string current = model();
  std::vector<LauncherResult> rows;
  double score = static_cast<double>(m_models.size());
  for (const auto& name : m_models) {
    if (!needle.empty() && !StringUtils::toLower(name).contains(needle)) {
      continue;
    }
    LauncherResult row;
    row.id = std::string(kModelPrefix) + name;
    row.title = name;
    row.subtitle = name == current ? i18n::tr("launcher.ai.current-model") : i18n::tr("launcher.ai.use-model");
    row.glyphName = name == current ? "circle-check" : "cpu";
    row.kind = i18n::tr("launcher.kinds.model");
    row.score = score--;
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<LauncherResult> AiProvider::query(std::string_view text) const {
  const std::string trimmed = StringUtils::trim(text);
  if (trimmed.size() < 3) {
    return {};
  }
  refreshModels(false); // so the row can name the model that would answer
  // Quick AI sits with the fallbacks under "Use … with", as in Raycast.
  return {askRow(trimmed, kFallbackScore, true)};
}

std::vector<LauncherResult> AiProvider::queryPrefixed(std::string_view text) const {
  const std::string trimmed = StringUtils::trim(text);
  refreshModels(false);
  if (m_session.has_value() && trimmed == m_session->view) {
    return answerRows();
  }
  std::vector<LauncherResult> results;
  if (trimmed.starts_with(kModelsKeyword)) {
    const std::string_view rest = std::string_view(trimmed).substr(kModelsKeyword.size());
    if (rest.empty() || rest.front() == ' ') {
      results = modelRows(rest);
      if (results.empty() && m_models.empty()) {
        if (auto status = statusRow(); status.has_value()) {
          results.push_back(std::move(*status));
        }
      }
      return results;
    }
  }
  if (auto status = statusRow(); status.has_value()) {
    results.push_back(std::move(*status));
  }
  if (!trimmed.empty()) {
    results.push_back(askRow(trimmed, kAskScore, false));
  }
  const std::string needle = StringUtils::toLower(trimmed);
  double order = 0.0;
  for (const auto& command : commands()) {
    double score = kCommandScore - order;
    order += 1.0;
    if (!needle.empty()) {
      score = FuzzyMatch::score(needle, StringUtils::toLower(i18n::tr(command.titleKey)));
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }
    results.push_back(commandRow(command, score));
  }
  if (needle.empty() || FuzzyMatch::isMatch(FuzzyMatch::score(needle, "choose model"))) {
    LauncherResult models;
    models.id = std::string(kModelsId);
    models.title = i18n::tr("launcher.ai.choose-model");
    const std::string current = model();
    models.subtitle = (current.empty() ? i18n::tr("launcher.ai.no-model") : current) + " · " + serviceName();
    models.glyphName = "cpu";
    models.kind = i18n::tr("launcher.kinds.command");
    models.score = kCommandScore - order - 1.0;
    results.push_back(std::move(models));
  }
  return results;
}

void AiProvider::stopStream() {
  if (m_session.has_value() && m_session->stream != 0 && m_httpClient != nullptr) {
    m_httpClient->cancelStream(m_session->stream);
    m_session->stream = 0;
    m_session->streaming = false;
  }
}

void AiProvider::ask(std::string view, std::string heading, std::string prompt) {
  stopStream();
  Session session;
  session.view = std::move(view);
  session.heading = std::move(heading);
  session.prompt = std::move(prompt);
  session.model = model();
  m_session = std::move(session);
  if (m_httpClient == nullptr) {
    m_session->error = i18n::tr("launcher.ai.no-client");
    return;
  }
  if (m_session->model.empty()) {
    m_session->error = i18n::tr("launcher.ai.no-model");
    return;
  }
  const Kind current = kind();
  const std::string key = apiKey();
  if (needsKey() && key.empty()) {
    m_session->error = i18n::tr("launcher.ai.no-key", "service", serviceName());
    return;
  }
  m_session->streaming = true;
  const auto consume = [this, current](std::string_view line) {
    if (const auto parsed = parseStreamLine(current, line); parsed.has_value()) {
      if (!parsed->error.empty()) {
        m_session->error = parsed->error;
      } else {
        m_session->answer += parsed->content;
      }
    }
  };
  m_session->stream = m_httpClient->startStream(
      chatRequest(current, baseUrl(), key, m_session->model, m_session->prompt),
      [this, consume](std::string_view chunk) {
        if (!m_session.has_value() || !m_session->streaming) {
          return;
        }
        m_session->lineBuffer.append(chunk);
        std::size_t start = 0;
        for (std::size_t end = m_session->lineBuffer.find('\n', start); end != std::string::npos;
             end = m_session->lineBuffer.find('\n', start)) {
          consume(std::string_view(m_session->lineBuffer).substr(start, end - start));
          start = end + 1;
        }
        m_session->lineBuffer.erase(0, start);
        if (m_onChanged) {
          m_onChanged();
        }
      },
      [this, consume](HttpStreamResult result) {
        if (!m_session.has_value()) {
          return;
        }
        consume(m_session->lineBuffer);
        m_session->lineBuffer.clear();
        m_session->streaming = false;
        m_session->stream = 0;
        if (m_session->error.empty() && m_session->answer.empty()) {
          if (!result.transportOk) {
            m_session->error = i18n::tr("launcher.ai.unreachable", "service", serviceName(), "url", baseUrl());
          } else if (result.status != 200) {
            m_session->error = "HTTP " + std::to_string(result.status);
          }
        }
        if (m_onChanged) {
          m_onChanged();
        }
      }
  );
  if (m_session->stream == 0) {
    m_session->streaming = false;
  }
}

bool AiProvider::activate(const LauncherResult& result) {
  if (result.id == kAskId) {
    const std::string question = StringUtils::trim(result.query.value_or(std::string()));
    if (question.empty()) {
      return false;
    }
    ask(question, question, question);
    if (m_requestQuery) {
      m_requestQuery(std::string(prefix()) + " " + question);
    }
    return false; // the launcher stays open on the answer
  }
  if (result.id == kAnswerId) {
    if (!m_session.has_value() || m_session->answer.empty() || m_clipboard == nullptr) {
      return false;
    }
    return m_clipboard->copyText(m_session->answer);
  }
  if (result.id == kModelsId) {
    refreshModels(true);
    if (m_requestQuery) {
      m_requestQuery(std::string(prefix()) + " " + std::string(kModelsKeyword) + " ");
    }
    return false;
  }
  if (result.id.starts_with(kModelPrefix)) {
    savePick(result.id.substr(kModelPrefix.size()));
    if (m_requestQuery) {
      m_requestQuery(std::string(prefix()) + " ");
    }
    return false;
  }
  if (result.id.starts_with(kCommandPrefix)) {
    const std::string commandId = result.id.substr(kCommandPrefix.size());
    const auto it = std::ranges::find(commands(), commandId, &Command::id);
    if (it == commands().end()) {
      return false;
    }
    const std::string input = clipboardInput();
    if (input.empty()) {
      return false;
    }
    std::string language;
    for (const auto& argument : result.arguments) {
      if (argument.id == "language") {
        language = argument.value;
      }
    }
    if (it->takesLanguage && language.empty()) {
      return false;
    }
    std::string heading = i18n::tr(it->titleKey);
    if (!language.empty()) {
      heading += " → " + language;
    }
    const std::string view = StringUtils::toLower(heading);
    ask(view, heading, fillPrompt(it->prompt, input, language));
    if (m_requestQuery) {
      m_requestQuery(std::string(prefix()) + " " + view);
    }
    return false;
  }
  return false;
}

std::string AiProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kAnswerId) {
    return i18n::tr("launcher.actions.copy-answer");
  }
  if (result.id == kAskId) {
    return i18n::tr("launcher.actions.ask-ai");
  }
  if (result.id.starts_with(kCommandPrefix)) {
    return i18n::tr("launcher.actions.run-command");
  }
  if (result.id.starts_with(kModelPrefix)) {
    return i18n::tr("launcher.actions.use-model");
  }
  return i18n::tr("launcher.actions.open");
}

std::vector<LauncherAction> AiProvider::actions(const LauncherResult& result) const {
  if (result.id != kAnswerId || !m_session.has_value()) {
    return {};
  }
  std::vector<LauncherAction> actions;
  if (m_session->streaming) {
    actions.push_back({.id = "stop", .label = i18n::tr("launcher.actions.stop-writing")});
  }
  actions.push_back({.id = "retry", .label = i18n::tr("launcher.actions.regenerate")});
  if (!m_session->answer.empty()) {
    actions.push_back({.id = "copy-question", .label = i18n::tr("launcher.actions.copy-question")});
  }
  return actions;
}

LauncherActionOutcome AiProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (result.id != kAnswerId || !m_session.has_value()) {
    return LauncherActionOutcome::Failed;
  }
  if (actionId == "stop") {
    stopStream();
    if (m_onChanged) {
      m_onChanged();
    }
    return LauncherActionOutcome::KeepOpen;
  }
  if (actionId == "retry") {
    Session copy = *m_session;
    ask(copy.view, copy.heading, copy.prompt);
    return LauncherActionOutcome::KeepOpen;
  }
  if (actionId == "copy-question" && m_clipboard != nullptr) {
    return m_clipboard->copyText(m_session->heading) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherResult> AiProvider::resultForId(std::string_view resultId) const {
  if (resultId.starts_with(kCommandPrefix)) {
    const std::string commandId(resultId.substr(kCommandPrefix.size()));
    const auto it = std::ranges::find(commands(), commandId, &Command::id);
    if (it != commands().end()) {
      return commandRow(*it, kCommandScore);
    }
  }
  return std::nullopt;
}
