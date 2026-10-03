#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <span>
#include <string_view>

class IpcService;

// System commands in the launcher (Raycast's System extension): toggle dark mode, Do Not Disturb,
// Wi-Fi or Bluetooth, take a screenshot, empty the trash, eject drives, … Most run the shell's own
// IPC commands in-process.
class SystemProvider : public LauncherProvider {
public:
  struct Command {
    std::string_view id;
    std::string_view glyph;
    // `noctalia msg` line run in-process; empty for the commands handled here.
    std::string_view ipc;
    // Run once the launcher has closed (screenshots and recordings would catch it otherwise).
    bool afterClose = false;
  };

  explicit SystemProvider(IpcService* ipc) : m_ipc(ipc) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "sys"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "System"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "settings"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;

  [[nodiscard]] static std::span<const Command> commands();

private:
  [[nodiscard]] std::vector<LauncherResult> search(std::string_view text, bool listAll) const;
  void run(const Command& command);

  IpcService* m_ipc = nullptr;
  Timer m_afterCloseTimer;
};
