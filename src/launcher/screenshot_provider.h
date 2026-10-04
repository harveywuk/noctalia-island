#pragma once

#include "launcher/launcher_provider.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

// Search Screenshots in the launcher (Raycast's command of the same name): the images in the
// screenshot folder, newest first, with a preview. Return opens one; the action panel copies the
// image, shows it in the folder, copies its path or moves it to the trash.
class ScreenshotProvider : public LauncherProvider {
public:
  struct Shot {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uintmax_t size = 0;
  };

  ScreenshotProvider(ClipboardService* clipboard, ConfigService* config);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "shot"; }
  [[nodiscard]] std::string_view id() const override { return "Screenshots"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "screenshot"; }
  [[nodiscard]] bool showsPreview() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;
  [[nodiscard]] std::optional<LauncherPreview> preview(const LauncherResult& result) const override;

  // The folder screenshots are saved to: shell.screenshot.directory, or the Pictures folder.
  [[nodiscard]] std::filesystem::path directory() const;
  // Image files directly inside `directory`, newest first; exposed for tests.
  [[nodiscard]] static std::vector<Shot> scan(const std::filesystem::path& directory);

private:
  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
};
