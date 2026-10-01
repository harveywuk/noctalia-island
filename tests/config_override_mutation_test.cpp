// Locks the atomic set-and-clear contract of ConfigService::mutateOverrides. Switching a calendar
// account to another provider writes the new keys and retires the ones the new provider does not own.
// Doing that as a clear followed by a set publishes an intermediate account that is neither shape and
// fails schema validation, so the mutation must reach the config exactly once.

#include "config/config_service.h"
#include "config/config_types.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_override_mutation: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  const CalendarConfig::Account* findAccount(const Config& config, std::string_view id) {
    const auto it = std::ranges::find(config.calendar.accounts, id, &CalendarConfig::Account::id);
    return it == config.calendar.accounts.end() ? nullptr : &*it;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-override-mutation-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", "\n");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  const std::vector<std::string> typePath{"calendar", "account", "feed", "type"};
  const std::vector<std::string> serverUrlPath{"calendar", "account", "feed", "server_url"};
  const std::vector<std::string> vdirPath{"calendar", "account", "feed", "path"};

  {
    ConfigService config;

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> create;
    create.emplace_back(std::vector<std::string>{"calendar", "enabled"}, true);
    create.emplace_back(typePath, std::string("ics"));
    create.emplace_back(serverUrlPath, std::string("https://example.com/calendar.ics"));
    expect(config.setOverrides(std::move(create)), "ics account writes");
    expect(config.hasOverride(serverUrlPath), "server_url stored for the ics account");

    int reloads = 0;
    config.addReloadCallback([&reloads]() { ++reloads; }, "mutation-test");

    // Switch the account to a local vdir directory. `server_url` is a hard error on a vdir account and
    // `path` is required, so both edits belong to the same commit.
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> switchToVdir;
    switchToVdir.emplace_back(typePath, std::string("vdir"));
    switchToVdir.emplace_back(vdirPath, std::string("/tmp/noctalia-vdir-mutation-test"));
    expect(config.mutateOverrides(switchToVdir, {serverUrlPath}, nullptr), "provider switch writes");

    expect(reloads == 1, "provider switch reaches the config exactly once");
    expect(!config.hasOverride(serverUrlPath), "server_url retired by the switch");
    expect(config.hasOverride(vdirPath), "path stored by the switch");
    expect(config.lastMutationError().empty(), "provider switch produced no mutation error");

    const CalendarConfig::Account* account = findAccount(config.config(), "feed");
    expect(account != nullptr, "account survives the switch");
    if (account != nullptr) {
      expect(account->type == "vdir", "account type is vdir");
      expect(account->path == "/tmp/noctalia-vdir-mutation-test", "account path is set");
      expect(account->serverUrl.empty(), "account server_url is cleared");
    }

    // A mutation that changes nothing must not commit, so consumers are not woken for a no-op.
    expect(config.mutateOverrides({}, {serverUrlPath}, nullptr), "clearing an absent key succeeds");
    expect(reloads == 1, "no-op mutation does not reach the config");
  }

  {
    ConfigService config;
    expect(config.createBarOverride("island"), "create an Island bar through regular bar management");
    expect(
        config.setOverrides(
            {{{"bar", "island", "presentation"}, std::string("island")},
             {{"bar", "island", "smart_auto_hide"}, true},
             {{"bar", "island", "island", "clock_seconds"}, true},
             {{"bar", "island", "island", "hover_widgets"}, std::vector<std::string>{"volume"}},
             {{"bar", "island", "island", "clock_size"}, 30.0},
             {{"bar", "island", "island", "activity_priority"}, std::string("media-downloads-timers")},
             {{"bar", "island", "island", "cycle_activities"}, true},
             {{"bar", "island", "island", "activity_cycle_seconds"}, std::int64_t{8}},
             {{"bar", "island", "island", "hover_open_delay_ms"}, std::int64_t{450}},
             {{"bar", "island", "island", "hover_close_delay_ms"}, std::int64_t{700}},
             {{"bar", "island", "island", "track_preview_seconds"}, std::int64_t{8}},
             {{"bar", "island", "island", "track_preview_monitor"}, std::string("focused")},
             {{"bar", "island", "island", "bluetooth_preview_monitor"}, std::string("DP-2")},
             {{"bar", "island", "island", "paused_media_seconds"}, std::int64_t{6}},
             {{"bar", "island", "island", "bluetooth_preview_seconds"}, std::int64_t{10}}}
        ),
        "write Island bar settings"
    );
    expect(config.createMonitorOverride("island", "DP-1"), "create an Island monitor override");
    expect(
        config.setOverrides(
            {{{"bar", "island", "monitor", "DP-1", "island", "clock_seconds"}, false},
             {{"bar", "island", "monitor", "DP-1", "island", "hover_widgets"}, std::vector<std::string>{}},
             {{"bar", "island", "monitor", "DP-1", "scale"}, 1.25},
             {{"bar", "island", "monitor", "DP-1", "island", "activity_priority"},
              std::string("downloads-media-timers")},
             {{"bar", "island", "monitor", "DP-1", "island", "cycle_activities"}, false},
             {{"bar", "island", "monitor", "DP-1", "island", "activity_cycle_seconds"}, std::int64_t{2}},
             {{"bar", "island", "monitor", "DP-1", "island", "hover_open_delay_ms"}, std::int64_t{0}},
             {{"bar", "island", "monitor", "DP-1", "island", "hover_close_delay_ms"}, std::int64_t{2000}},
             {{"bar", "island", "monitor", "DP-1", "island", "track_preview_seconds"}, std::int64_t{0}},
             {{"bar", "island", "monitor", "DP-1", "island", "track_preview_monitor"}, std::string("DP-1")},
             {{"bar", "island", "monitor", "DP-1", "island", "paused_media_seconds"}, std::int64_t{2}},
             {{"bar", "island", "monitor", "DP-1", "island", "bluetooth_preview_seconds"}, std::int64_t{0}},
             {{"bar", "island", "monitor", "DP-1", "island", "reveal_on_track_change"}, false}}
        ),
        "write partial Island monitor override"
    );
    WaylandOutput output;
    output.connectorName = "DP-1";
    const auto find = [&]() -> const BarConfig& {
      return *std::ranges::find(config.config().bars, "island", &BarConfig::name);
    };
    auto resolved = ConfigService::resolveForOutput(find(), output);
    expect(
        resolved.presentation == BarPresentation::Island && resolved.smartAutoHide,
        "inherit bar presentation and smart hide"
    );
    expect(
        !resolved.island.clockSeconds && resolved.island.hoverWidgets.empty(),
        "false and empty monitor overrides survive"
    );
    expect(
        resolved.island.clockSize == 30 && resolved.scale == 1.25F,
        "unmodified content inherits and common scale overrides"
    );
    expect(
        resolved.island.activityPriority == IslandActivityPriority::DownloadsMediaTimers
            && !resolved.island.cycleActivities
            && resolved.island.activityCycleSeconds == 2,
        "monitor activity order, disabled cycling and interval override parent"
    );
    expect(
        config.clearOverrides({{"bar", "island", "monitor", "DP-1", "island", "cycle_activities"}}, nullptr),
        "reset monitor cycling"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.cycleActivities, "cycling reset inherits enabled parent"
    );
    expect(
        config.clearOverrides(
            {{"bar", "island", "monitor", "DP-1", "island", "activity_priority"},
             {"bar", "island", "monitor", "DP-1", "island", "activity_cycle_seconds"}},
            nullptr
        ),
        "reset monitor activity order and interval"
    );
    auto activities = ConfigService::resolveForOutput(find(), output).island;
    expect(
        activities.activityPriority == IslandActivityPriority::MediaDownloadsTimers
            && activities.activityCycleSeconds == 8,
        "activity settings inherit parent"
    );
    expect(
        config.clearOverrides(
            {{"bar", "island", "island", "activity_priority"},
             {"bar", "island", "island", "cycle_activities"},
             {"bar", "island", "island", "activity_cycle_seconds"}},
            nullptr
        ),
        "reset bar activity settings"
    );
    activities = ConfigService::resolveForOutput(find(), output).island;
    expect(
        activities.activityPriority == IslandActivityPriority::TimersDownloadsMedia
            && !activities.cycleActivities
            && activities.activityCycleSeconds == 5,
        "activity settings reset to defaults"
    );
    expect(
        resolved.island.hoverOpenDelayMs == 0 && resolved.island.hoverCloseDelayMs == 2000,
        "monitor hover delays accept immediate and maximum values"
    );
    expect(
        config.clearOverrides({{"bar", "island", "monitor", "DP-1", "island", "hover_open_delay_ms"}}, nullptr),
        "reset monitor open delay"
    );
    resolved = ConfigService::resolveForOutput(find(), output);
    expect(
        resolved.island.hoverOpenDelayMs == 450 && resolved.island.hoverCloseDelayMs == 2000,
        "reset open delay inherits parent without changing close delay"
    );
    expect(
        config.clearOverrides({{"bar", "island", "island", "hover_open_delay_ms"}}, nullptr), "reset bar open delay"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.hoverOpenDelayMs == 110,
        "bar reset restores the default open delay"
    );
    expect(
        config.clearOverrides({{"bar", "island", "monitor", "DP-1", "island", "hover_close_delay_ms"}}, nullptr),
        "reset monitor close delay"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.hoverCloseDelayMs == 700,
        "close delay reset inherits parent"
    );
    expect(
        config.clearOverrides({{"bar", "island", "island", "hover_close_delay_ms"}}, nullptr), "reset bar close delay"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.hoverCloseDelayMs == 180,
        "bar reset restores the default close delay"
    );
    const std::vector<std::string> seconds{"bar", "island", "monitor", "DP-1", "island", "clock_seconds"};
    expect(
        resolved.island.trackPreviewMonitor == "DP-1" && resolved.island.bluetoothPreviewMonitor == "DP-2",
        "monitor targeting overrides and inheritance"
    );
    expect(
        config.clearOverrides({{"bar", "island", "monitor", "DP-1", "island", "track_preview_monitor"}}, nullptr),
        "reset monitor targeting"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.trackPreviewMonitor == "focused",
        "target reset inherits parent routing"
    );
    expect(
        resolved.island.trackPreviewSeconds == 0
            && resolved.island.pausedMediaSeconds == 2
            && resolved.island.bluetoothPreviewSeconds == 0
            && !resolved.island.revealOnTrackChange,
        "monitor durations, zero and false override the parent"
    );
    const std::vector<std::string> preview{"bar", "island", "monitor", "DP-1", "island", "track_preview_seconds"};
    expect(config.clearOverrides({preview}, nullptr), "reset monitor preview duration");
    resolved = ConfigService::resolveForOutput(find(), output);
    expect(
        resolved.island.trackPreviewSeconds == 8 && resolved.island.pausedMediaSeconds == 2,
        "reset inherits parent duration and preserves other monitor settings"
    );
    expect(
        config.clearOverrides({{"bar", "island", "island", "track_preview_seconds"}}, nullptr),
        "reset parent preview duration"
    );
    expect(
        ConfigService::resolveForOutput(find(), output).island.trackPreviewSeconds == 5,
        "reset parent restores the default duration"
    );
    expect(config.clearOverrides({seconds}, nullptr), "reset one Island monitor setting");
    resolved = ConfigService::resolveForOutput(find(), output);
    expect(
        resolved.island.clockSeconds && resolved.island.hoverWidgets.empty(),
        "reset inherits without losing other monitor choices"
    );
    expect(config.renameBarOverride("island", "capsule"), "rename Island bar");
    expect(
        std::ranges::find(config.config().bars, "capsule", &BarConfig::name)->presentation == BarPresentation::Island,
        "rename retains Island presentation"
    );
    expect(config.deleteBarOverride("capsule"), "delete Island bar");
  }
  {
    ConfigService config;
    const std::vector<std::string> enabled{"shell", "hyprland_workspaces", "3", "enabled"};
    expect(config.setOverrides({{enabled, false}}), "add disabled workspace entry");
    expect(config.config().shell.hyprlandWorkspaces.size() == 1, "disabled workspace entry is retained");
    expect(
        config.setOverrides(
            {{enabled, true},
             {{"shell", "hyprland_workspaces", "3", "label"}, std::string("Writing")},
             {{"shell", "hyprland_workspaces", "3", "icon"}, std::string("★")},
             {{"shell", "hyprland_workspaces", "3", "monitor"}, std::string("DP-2")},
             {{"shell", "hyprland_workspaces", "3", "persistent"}, true}}
        ),
        "save workspace preferences"
    );
    ConfigService restored;
    expect(
        restored.config().shell.hyprlandWorkspaces == config.config().shell.hyprlandWorkspaces,
        "workspace preferences survive shell restart"
    );
    expect(config.clearOverrides({{"shell", "hyprland_workspaces", "3"}}, nullptr), "remove workspace overrides");
    expect(config.config().shell.hyprlandWorkspaces.empty(), "removed entry leaves no phantom workspace");
  }
  {
    ConfigService config;
    expect(config.setOverrides({{{"shell", "hyprland_keybinds", "test", "enabled"}, false}}), "add disabled shortcut");
    expect(config.config().shell.hyprlandKeybinds.size() == 1, "retain disabled shortcut");
    expect(
        config.setOverrides(
            {{{"shell", "hyprland_keybinds", "test", "chord"}, std::string("SUPER+F")},
             {{"shell", "hyprland_keybinds", "test", "enabled"}, true},
             {{"shell", "hyprland_keybinds", "test", "replace_existing"}, true}}
        ),
        "save compositor shortcut"
    );
    ConfigService restored;
    expect(
        restored.config().shell.hyprlandKeybinds == config.config().shell.hyprlandKeybinds, "shortcut survives restart"
    );
    expect(config.clearOverrides({{"shell", "hyprland_keybinds", "test"}}, nullptr), "remove shortcut overrides");
    expect(config.config().shell.hyprlandKeybinds.empty(), "removed shortcut leaves no entry");
  }
  {
    ConfigService config;
    const std::vector<std::string> base{"shell", "session", "startup_apps", "test"};
    auto path = [&](const char* key) {
      auto p = base;
      p.push_back(key);
      return p;
    };
    expect(
        config.setOverrides({{path("enabled"), false}, {path("kind"), std::string("command")}}),
        "add disabled startup command"
    );
    expect(config.config().shell.session.startupApps.size() == 1, "keep disabled startup entry");
    expect(
        config.setOverrides(
            {{path("command"), std::string("printf 'test' > /tmp/example")},
             {path("delay_seconds"), std::int64_t(25)},
             {path("enabled"), true}}
        ),
        "edit startup command"
    );
    ConfigService restored;
    expect(
        restored.config().shell.session.startupApps == config.config().shell.session.startupApps,
        "startup entry persists"
    );
    expect(config.clearOverrides({base}, nullptr), "remove startup overrides");
    expect(config.config().shell.session.startupApps.empty(), "startup entry removed");
  }
  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::println("config_override_mutation_test passed");
    return 0;
  }
  return 1;
}
