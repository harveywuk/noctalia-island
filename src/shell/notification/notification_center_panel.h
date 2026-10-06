#pragma once

#include "shell/control_center/tabs/notifications_tab.h"
#include "shell/panel/panel.h"

#include <functional>

class CompositorPlatform;
class Flex;
class Button;
class NotificationManager;

// App stacks expand inside the Island. Without an Island, retain the floating history column.
class NotificationCenterPanel : public Panel {
public:
  NotificationCenterPanel(NotificationManager* notifications, CompositorPlatform* platform);

  // Runs as the panel opens; the app uses it to clear banners, which macOS folds into the centre.
  std::function<void()> onOpened;

  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] float islandWidth(float availableWidth) const override;
  [[nodiscard]] float islandHeight(float availableHeight) const override;
  [[nodiscard]] InputArea* initialFocusArea() const override;
  void scrollFocusedInputIntoView(InputArea* area) override;
  [[nodiscard]] bool fillsHeight() const noexcept override { return true; }
  // Cards float straight over the desktop, with no panel sheet behind them.
  [[nodiscard]] bool hasDecoration() const override { return false; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  NotificationsTab m_history;
  Flex* m_root = nullptr;
  Flex* m_header = nullptr;
  Flex* m_body = nullptr;
  Button* m_close = nullptr;
};
