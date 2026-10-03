#pragma once

#include "launcher/launcher_provider.h"

#include <filesystem>
#include <string>
#include <vector>

class ConfigService;

// Script commands: executable scripts with Raycast's metadata comments (`# @raycast.title …`, or the
// same keys under `@noctalia.`), run from the launcher. Arguments are typed after the title
// ("translate hello world"). Existing Raycast script commands work unchanged.
class ScriptProvider : public LauncherProvider {
public:
  struct Argument {
    std::string placeholder;
    bool optional = false;
  };

  enum class Mode { Silent, Compact, FullOutput };

  struct Script {
    std::filesystem::path path;
    std::string title;
    std::string description;
    std::string packageName;
    std::string icon;
    Mode mode = Mode::Compact;
    std::vector<Argument> arguments;
  };

  explicit ScriptProvider(ConfigService* config);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "script"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Scripts"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "script"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }

  void setQueryRequestedCallback(std::function<void(std::string)> callback) override {
    m_requestQuery = std::move(callback);
  }
  void reset() override { m_scanned = false; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherResult> resultForId(std::string_view resultId) const override;

  // Reads a script's metadata comments. Nullopt when it has no title. Exposed for tests.
  [[nodiscard]] static std::optional<Script> parse(const std::filesystem::path& path, std::string_view contents);
  // Splits typed arguments: the last argument takes the rest of the line; "quotes" group words.
  [[nodiscard]] static std::vector<std::string> splitArguments(std::string_view text, std::size_t count);

private:
  [[nodiscard]] const std::vector<Script>& scripts() const;
  [[nodiscard]] std::vector<LauncherResult> search(std::string_view text, bool listAll) const;
  [[nodiscard]] const Script* scriptFor(std::string_view resultId) const;
  void run(const Script& script, std::vector<std::string> arguments) const;

  ConfigService* m_config = nullptr;
  std::function<void(std::string)> m_requestQuery;
  mutable std::vector<Script> m_scripts;
  mutable bool m_scanned = false;
};
