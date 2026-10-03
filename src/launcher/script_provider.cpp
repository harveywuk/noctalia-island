#include "launcher/script_provider.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "notification/notifications.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unistd.h>

namespace {

  constexpr Logger kLog("launcher-scripts");
  constexpr std::size_t kHeaderLines = 64;
  constexpr std::size_t kMaxScriptBytes = 256 * 1024;
  constexpr std::size_t kMaxOutputChars = 2000;
  constexpr double kArgumentScore = 8000.0;

  [[nodiscard]] std::vector<std::filesystem::path> scriptDirectories(const ConfigService* config) {
    std::vector<std::filesystem::path> dirs;
    if (config != nullptr) {
      for (const auto& dir : config->config().shell.launcher.scriptDirectories) {
        if (!StringUtils::isBlank(dir)) {
          dirs.push_back(FileUtils::expandUserPath(dir));
        }
      }
    }
    if (dirs.empty()) {
      const std::string base = FileUtils::configDir();
      if (!base.empty()) {
        dirs.emplace_back(std::filesystem::path(base) / "scripts");
      }
    }
    return dirs;
  }

  [[nodiscard]] bool isShortSymbol(std::string_view icon) {
    // An emoji or a character or two, as Raycast allows; anything longer is a file name.
    return !icon.empty() && icon.size() <= 8 && !icon.contains('.') && !icon.contains('/');
  }

  [[nodiscard]] std::string lastLine(std::string_view text) {
    const std::string trimmed = StringUtils::trim(text);
    const auto newline = trimmed.rfind('\n');
    return newline == std::string::npos ? trimmed : StringUtils::trim(std::string_view(trimmed).substr(newline + 1));
  }

} // namespace

ScriptProvider::ScriptProvider(ConfigService* config) : m_config(config) {}

std::string ScriptProvider::displayName() const { return i18n::tr("launcher.providers.scripts.title"); }

std::optional<ScriptProvider::Script>
ScriptProvider::parse(const std::filesystem::path& path, std::string_view contents) {
  Script script;
  script.path = path;
  std::istringstream stream{std::string(contents)};
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(stream, line) && lineNumber++ < kHeaderLines) {
    std::size_t at = line.find("@raycast.");
    std::size_t keyStart = at + 9;
    if (at == std::string::npos) {
      at = line.find("@noctalia.");
      keyStart = at + 10;
    }
    if (at == std::string::npos) {
      continue;
    }
    const std::size_t keyEnd = line.find_first_of(" \t", keyStart);
    const std::string key = line.substr(keyStart, keyEnd == std::string::npos ? std::string::npos : keyEnd - keyStart);
    const std::string value = keyEnd == std::string::npos ? std::string() : StringUtils::trim(line.substr(keyEnd));
    if (key == "title") {
      script.title = value;
    } else if (key == "description") {
      script.description = value;
    } else if (key == "packageName") {
      script.packageName = value;
    } else if (key == "icon") {
      script.icon = value;
    } else if (key == "mode") {
      if (value == "silent") {
        script.mode = Mode::Silent;
      } else if (value == "fullOutput") {
        script.mode = Mode::FullOutput;
      } else {
        // compact and inline: one line of output.
        script.mode = Mode::Compact;
      }
    } else if (key.starts_with("argument") && key.size() == 9 && key[8] >= '1' && key[8] <= '3') {
      Argument argument;
      try {
        const auto json = nlohmann::json::parse(value);
        argument.placeholder = json.value("placeholder", "");
        argument.optional = json.value("optional", false);
      } catch (const nlohmann::json::exception&) {
        argument.placeholder = value;
      }
      const std::size_t index = static_cast<std::size_t>(key[8] - '1');
      if (script.arguments.size() <= index) {
        script.arguments.resize(index + 1);
      }
      script.arguments[index] = std::move(argument);
    }
  }
  if (script.title.empty()) {
    return std::nullopt;
  }
  return script;
}

std::vector<std::string> ScriptProvider::splitArguments(std::string_view text, std::size_t count) {
  std::vector<std::string> out;
  if (count == 0) {
    return out;
  }
  std::string_view rest = StringUtils::trimLeftView(text);
  while (!rest.empty() && out.size() + 1 < count) {
    std::string word;
    if (rest.front() == '"') {
      const auto close = rest.find('"', 1);
      word = std::string(rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
      rest = close == std::string_view::npos ? std::string_view{} : rest.substr(close + 1);
    } else {
      const auto space = rest.find(' ');
      word = std::string(rest.substr(0, space));
      rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    }
    out.push_back(std::move(word));
    rest = StringUtils::trimLeftView(rest);
  }
  std::string last = StringUtils::trim(rest);
  if (last.size() >= 2 && last.front() == '"' && last.back() == '"') {
    last = last.substr(1, last.size() - 2);
  }
  if (!last.empty()) {
    out.push_back(std::move(last));
  }
  return out;
}

const std::vector<ScriptProvider::Script>& ScriptProvider::scripts() const {
  if (m_scanned) {
    return m_scripts;
  }
  m_scanned = true;
  m_scripts.clear();
  for (const auto& dir : scriptDirectories(m_config)) {
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
      if (!item.is_regular_file(ec) || item.file_size(ec) > kMaxScriptBytes) {
        continue;
      }
      if (::access(item.path().c_str(), X_OK) != 0) {
        continue;
      }
      std::ifstream file(item.path());
      std::string head;
      std::string line;
      for (std::size_t i = 0; i < kHeaderLines && std::getline(file, line); ++i) {
        head += line;
        head += '\n';
      }
      if (auto script = parse(item.path(), head)) {
        m_scripts.push_back(std::move(*script));
      }
    }
  }
  std::ranges::sort(m_scripts, {}, [](const Script& s) { return StringUtils::toLower(s.title); });
  return m_scripts;
}

const ScriptProvider::Script* ScriptProvider::scriptFor(std::string_view resultId) const {
  for (const auto& script : scripts()) {
    if (script.path.string() == resultId) {
      return &script;
    }
  }
  return nullptr;
}

std::vector<LauncherResult> ScriptProvider::search(std::string_view text, bool listAll) const {
  const std::string trimmed = StringUtils::trim(text);
  const std::string needle = StringUtils::toLower(trimmed);
  if (needle.empty() && !listAll) {
    return {};
  }
  std::vector<LauncherResult> results;
  for (const auto& script : scripts()) {
    const std::string title = StringUtils::toLower(script.title);
    std::optional<std::string> arguments;
    double score = 0.0;
    if (!script.arguments.empty() && needle.starts_with(title + " ")) {
      arguments = StringUtils::trim(std::string_view(trimmed).substr(title.size() + 1));
      score = kArgumentScore;
    } else if (!needle.empty()) {
      score = FuzzyMatch::score(needle, title);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }

    LauncherResult result;
    result.id = script.path.string();
    result.title = script.title;
    if (arguments.has_value() && !arguments->empty()) {
      result.query = *arguments;
      result.subtitle = *arguments;
    } else if (!script.arguments.empty()) {
      std::string placeholders;
      for (const auto& argument : script.arguments) {
        if (!placeholders.empty()) {
          placeholders += ' ';
        }
        placeholders += "‹" + (argument.placeholder.empty() ? std::string("…") : argument.placeholder) + "›";
      }
      result.subtitle = placeholders;
    } else {
      result.subtitle = !script.description.empty() ? script.description : script.packageName;
    }
    if (isShortSymbol(script.icon)) {
      result.badge = script.icon;
    } else if (!script.icon.empty()) {
      const std::filesystem::path icon = script.icon;
      result.iconPath = (icon.is_absolute() ? icon : script.path.parent_path() / icon).string();
    }
    result.glyphName = "script";
    result.kind = i18n::tr("launcher.kinds.script");
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> ScriptProvider::query(std::string_view text) const { return search(text, false); }

std::vector<LauncherResult> ScriptProvider::queryPrefixed(std::string_view text) const { return search(text, true); }

std::optional<LauncherResult> ScriptProvider::resultForId(std::string_view resultId) const {
  for (auto& result : search({}, true)) {
    if (result.id == resultId) {
      return result;
    }
  }
  return std::nullopt;
}

void ScriptProvider::run(const Script& script, std::vector<std::string> arguments) const {
  // Run from the script's own folder, as Raycast does.
  std::vector<std::string> argv = {
      "/bin/sh", "-c", "cd \"$(dirname \"$0\")\" && exec \"$0\" \"$@\"", script.path.string()
  };
  for (auto& argument : arguments) {
    argv.push_back(std::move(argument));
  }
  const bool started = process::runAsync(
      argv,
      process::RunCallbacks{
          .onExit = [title = script.title, mode = script.mode](process::RunResult result) {
            DeferredCall::callLater([title, mode, result = std::move(result)]() {
              if (!result) {
                const std::string error = lastLine(result.err.empty() ? result.out : result.err);
                notify::error(
                    "Noctalia", i18n::tr("launcher.scripts.failed", "title", title),
                    error.empty() ? i18n::tr("launcher.scripts.exit-code", "code", result.exitCode) : error
                );
                return;
              }
              if (mode == Mode::Silent) {
                return;
              }
              std::string output = mode == Mode::Compact ? lastLine(result.out) : StringUtils::trim(result.out);
              if (output.empty()) {
                return;
              }
              if (output.size() > kMaxOutputChars) {
                output = StringUtils::truncateUtf8(output, kMaxOutputChars) + "…";
              }
              notify::info("Noctalia", title, output);
            });
          },
      }
  );
  if (!started) {
    kLog.warn("failed to start script {}", script.path.string());
  }
}

bool ScriptProvider::activate(const LauncherResult& result) {
  const Script* script = scriptFor(result.id);
  if (script == nullptr) {
    return false;
  }
  const std::size_t required = static_cast<std::size_t>(
      std::ranges::count_if(script->arguments, [](const Argument& argument) { return !argument.optional; })
  );
  std::vector<std::string> arguments = splitArguments(result.query.value_or(""), script->arguments.size());
  if (arguments.size() < required) {
    // Arguments still to type: put the title in the field so they can follow it.
    if (m_requestQuery) {
      m_requestQuery(script->title + " ");
    }
    return false;
  }
  run(*script, std::move(arguments));
  return true;
}

std::string ScriptProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.run-script");
}

std::vector<LauncherAction> ScriptProvider::actions(const LauncherResult& /*result*/) const {
  return {
      {.id = "edit", .label = i18n::tr("launcher.actions.edit-script")},
      {.id = "show", .label = i18n::tr("launcher.actions.show-in-folder")},
  };
}

LauncherActionOutcome ScriptProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  const Script* script = scriptFor(result.id);
  if (script == nullptr) {
    return LauncherActionOutcome::Failed;
  }
  if (actionId == "edit") {
    return launcher_util::openUri(script->path.string()) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  if (actionId == "show") {
    return launcher_util::showInFolder(script->path) ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}
