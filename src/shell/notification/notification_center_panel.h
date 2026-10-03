#pragma once

#include "shell/control_center/tabs/notifications_tab.h"
#include "shell/panel/panel.h"

#include <functional>

class CompositorPlatform;
class Flex;
class NotificationManager;

// Notification Centre as on macOS: its own full-height column on the right edge of the screen,
// holding the notification history stacked by app. It replaces the Control Center's
// notifications tab; opening that tab lands here.
class NotificationCenterPanel : public Panel {
public:
  NotificationCenterPanel(NotificationManager* notifications, CompositorPlatform* platform);

  // Runs as the panel opens; the app uses it to clear banners, which macOS folds into the centre.
  std::function<void()> onOpened;

  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool fillsHeight() const noexcept override { return true; }
  // Cards float straight over the desktop, with no panel sheet behind them.
  [[nodiscard]] bool hasDecoration() const override { return false; }
  [[nodiscard]] bool islandHostable() const noexcept override { return false; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  NotificationsTab m_history;
  Flex* m_root = nullptr;
  Flex* m_header = nullptr;
  Flex* m_body = nullptr;
};
