#pragma once
#include "compositors/hyprland/hyprland_event_handler.h"
#include "config/config_types.h"
#include "core/timer_manager.h"

#include <functional>
#include <optional>

namespace compositors::hyprland {
  struct DisplayInfo {
    std::string output, description, mode, colorMode;
    std::vector<std::string> modes;
    int x = 0, y = 0, transform = 0, bitDepth = 8;
    float scale = 1, sdrBrightness = 1, sdrSaturation = 1;
    bool vrrActive = false;
  };
  std::vector<DisplayInfo> parseDisplays(std::string_view payload);
  std::string displayProblem(const HyprlandDisplayConfig&, const DisplayInfo&);
  std::string displayCommand(const HyprlandDisplayConfig&, const DisplayInfo&);

  class HyprlandDisplays final : public HyprlandEventHandler {
  public:
    explicit HyprlandDisplays(HyprlandRuntime&);
    ~HyprlandDisplays() override;
    void sync(const std::vector<HyprlandDisplayConfig>&);
    std::vector<DisplayInfo> displays() const;
    bool preview(std::vector<HyprlandDisplayConfig>);
    bool keep(const std::function<bool(const std::vector<HyprlandDisplayConfig>&)>& save);
    void revert();
    bool previewing() const { return m_preview.has_value(); }
    int secondsLeft() const;
    const std::string& error() const { return m_error; }
    std::function<void()> changed;
    void handleEvent(std::string_view event, std::string_view) override;
    void notifyCleanup() override;
    void notifyChanged() override;

  private:
    bool apply();
    void restore();
    std::vector<HyprlandDisplayConfig> m_saved;
    std::optional<std::vector<HyprlandDisplayConfig>> m_preview;
    std::chrono::steady_clock::time_point m_deadline;
    Timer m_confirmation, m_applyTimer;
    std::string m_error;
  };
} // namespace compositors::hyprland
