#pragma once

#include "launcher/launcher_provider.h"
#include "system/desktop_entry.h"
#include "wayland/wayland_toplevels.h"

#include <cstdint>
#include <vector>

class ClipboardService;
class ConfigService;
class CompositorPlatform;

class AppProvider : public LauncherProvider {
public:
  explicit AppProvider(
      ConfigService* config, CompositorPlatform* platform = nullptr, ClipboardService* clipboard = nullptr
  );
  [[nodiscard]] static std::string actionResultId(std::string_view desktopEntryPath, std::string_view desktopActionId);

  [[nodiscard]] std::string_view defaultPrefix() const override { return ""; }
  [[nodiscard]] bool allowCustomPrefix() const override { return false; }
  [[nodiscard]] std::string_view id() const override { return "Applications"; }
  [[nodiscard]] bool supportsAliases() const override { return true; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "app-window"; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  void initialize() override;

  [[nodiscard]] std::vector<LauncherCategory> categories() const override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;
  // Raycast's application actions: Quit Application while it has windows, Copy Name.
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

private:
  void refreshEntriesIfNeeded() const;
  [[nodiscard]] const DesktopEntry* entryFor(const LauncherResult& result) const;
  // The open windows of the application behind `entry`, matched the way the dock does.
  [[nodiscard]] std::vector<ToplevelInfo> windowsFor(const DesktopEntry& entry) const;

  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ClipboardService* m_clipboard = nullptr;
  mutable std::vector<DesktopEntry> m_entries;
  mutable std::uint64_t m_entriesVersion = 0;
};
