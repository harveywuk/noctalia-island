#include "config/config_service.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

namespace {
  int failures = 0;
  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      ++failures;
    }
  }
  void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }
} // namespace
int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("noctalia-backup-test-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto configFile = root / "config/noctalia/config.toml";
  write(configFile, "[shell]\nfont_family = 'sans-serif'\n");
  setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  const std::vector<std::string> all{config_backup::sections.begin(), config_backup::sections.end()};
  std::string error, undo;
  ConfigService config;
  expect(config.createBarOverride("island"), "create initial bar");
  expect(
      config.setOverrides(
          {{{"shell", "font_family"}, std::string("serif")},
           {{"bar", "island", "presentation"}, std::string("island")},
           {{"bar", "island", "monitor", "TEST-2", "enabled"}, false}}
      ),
      "seed appearance and monitor override"
  );
  const auto saved = config.createBackup("Working setup / α", error);
  expect(saved.has_value(), "save: " + error);
  if (!saved)
    return 1;
  const auto file = root / "state/noctalia/backups" / (saved->id + ".toml");
  expect((fs::status(file).permissions() & fs::perms::group_all) == fs::perms::none, "backup is private");
  expect(config.listBackups(error).size() == 1 && error.empty(), "backup listed");
  expect(
      config.setOverrides(
          {{{"shell", "font_family"}, std::string("monospace")},
           {{"shell", "hyprland_input", "pointer_sensitivity"}, 0.4}}
      ),
      "change independent sections"
  );
  expect(config.createBarOverride("new-bar"), "add a bar after backup");
  expect(config.setOverride({"bar", "island", "monitor", "TEST-2", "smart_auto_hide"}, true), "new monitor override");
  auto plan = config.previewBackup(saved->id, {"appearance"}, error);
  expect(plan.has_value(), "preview: " + error);
  if (plan) {
    expect(!plan->changes.empty(), "preview describes changes");
    expect(config.restoreBackup(*plan, undo, error), "restore appearance: " + error);
    expect(!undo.empty(), "automatic undo created first");
    expect(config.config().shell.fontFamily == "serif", "appearance restored");
    expect(config.hasOverride({"shell", "hyprland_input", "pointer_sensitivity"}), "unselected input preserved");
    expect(config.isOverrideOnlyBar("new-bar"), "unselected new bar preserved");
  }
  auto undoPlan = config.previewBackup(undo, {"appearance"}, error);
  expect(undoPlan.has_value(), "preview undo: " + error);
  if (undoPlan)
    expect(config.restoreBackup(*undoPlan, undo, error), "undo restore: " + error);
  expect(config.config().shell.fontFamily == "monospace", "undo restores exact previous value");
  plan = config.previewBackup(saved->id, {"bars"}, error);
  expect(plan.has_value(), "preview bars: " + error);
  if (plan)
    expect(config.restoreBackup(*plan, undo, error), "restore bars: " + error);
  expect(!config.isOverrideOnlyBar("new-bar"), "restore removes newly added override-only bar");
  expect(
      !config.hasOverride({"bar", "island", "monitor", "TEST-2", "smart_auto_hide"}), "monitor inheritance restored"
  );
  expect(config.hasOverride({"bar", "island", "monitor", "TEST-2", "enabled"}), "original monitor override retained");
  plan = config.previewBackup(saved->id, all, error);
  expect(plan.has_value(), "full preview: " + error);
  if (plan) {
    expect(config.setOverride({"shell", "font_family"}, std::string("new value")), "change after review");
    expect(!config.restoreBackup(*plan, undo, error), "stale preview rejected");
    expect(config.config().shell.fontFamily == "new value", "stale preview does not mutate config");
  }
  // An unrelated hand-authored change must not block a scoped restore.
  write(configFile, "[shell]\nfont_family = 'sans-serif'\n[notification]\nenable_daemon = false\n");
  config.forceReload();
  expect(
      config.previewBackup(saved->id, {"appearance"}, error).has_value(), "unselected base changes allowed: " + error
  );
  expect(!config.previewBackup(saved->id, {"other"}, error), "changed base section refused");
  write(configFile, "[shell]\nfont_family = 'different base'\n");
  config.forceReload();
  expect(!config.previewBackup(saved->id, {"appearance"}, error), "changed hand-written appearance refused");
  expect(!config.previewBackup("../bad", all, error), "path traversal refused");
  expect(!config.previewBackup(saved->id, {}, error), "empty selection refused");
  expect(!config.previewBackup(saved->id, {"invalid"}, error), "unknown section refused");
  expect(!config.createBackup("", error), "empty name refused");
  write(configFile, "[shell]\nfont_family = 'sans-serif'\n");
  config.forceReload();
  plan = config.previewBackup(saved->id, all, error);
  expect(plan.has_value(), "full restore ready: " + error);
  if (plan) {
    const auto original = config.config().shell.fontFamily;
    auto reloads = std::make_shared<int>(0);
    config.addReloadCallback([reloads] { ++*reloads; }, "backup-test");
    expect(config.restoreBackup(*plan, undo, error), "full restore: " + error);
    expect(*reloads == 1, "restore commits once");
    expect(config.config().shell.fontFamily == "serif" && original != "serif", "full restore applies saved value");
    expect(
        !config.hasOverride({"shell", "hyprland_input", "pointer_sensitivity"}),
        "full restore removes later input override"
    );
    const auto unchanged = config.previewBackup(saved->id, all, error);
    expect(unchanged && unchanged->changes.empty(), "restored snapshot has no changes");
    if (unchanged) {
      const auto count = config.listBackups(error).size();
      expect(config.restoreBackup(*unchanged, undo, error), "no-op restore succeeds");
      expect(
          undo.empty() && config.listBackups(error).size() == count && *reloads == 1,
          "no-op creates no backup or reload"
      );
    }
  }
  auto snapshot = toml::parse_file(file.string());
  snapshot.insert_or_assign("format", 999);
  std::ofstream(file) << snapshot;
  expect(!config.previewBackup(saved->id, all, error), "future format refused");
  const auto bad = file.parent_path() / "123-456.toml";
  write(bad, "invalid = [");
  auto listed = config.listBackups(error);
  expect(!error.empty() && fs::exists(bad), "invalid backup preserved and reported");
  fs::remove_all(root);
  std::println(
      "{}: scoped restore, exact undo, monitor inheritance, stale previews and invalid backups",
      failures ? "FAIL" : "PASS"
  );
  return failures ? 1 : 0;
}
