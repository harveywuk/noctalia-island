#pragma once

#include "shell/desktop/desktop_battery_model.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"

#include <array>

class DesktopBatteryRing;
class Label;

class DesktopBatteriesWidget final : public DesktopWidget {
public:
  DesktopBatteriesWidget(
      DesktopWidgetRuntimeServices services, desktop_cards::Size size, std::vector<std::string> hiddenDevices
  );
  void create() override;

private:
  struct Slot {
    DesktopBatteryRing* ring = nullptr;
    Label* name = nullptr;
    Label* percentage = nullptr;
    Label* status = nullptr;
  };
  bool usesCardLayout() const noexcept override { return true; }
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool refresh();
  DesktopWidgetRuntimeServices m_services;
  desktop_cards::Size m_size;
  std::vector<std::string> m_hidden;
  std::vector<desktop_batteries::Device> m_devices;
  bool m_hasDevices = false;
  std::array<Slot, 4> m_slots{};
  Label* m_heading = nullptr;
  Label* m_empty = nullptr;
};
