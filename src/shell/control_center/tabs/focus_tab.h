#pragma once

#include "shell/control_center/tab.h"

#include <array>

class NotificationManager;
class Button;

class FocusTab final : public Tab {
public:
  explicit FocusTab(NotificationManager* notifications) : m_notifications(notifications) {}
  std::unique_ptr<Flex> create() override;
  void onClose() override;

private:
  void syncStatus();
  void doLayout(Renderer&, float width, float height) override;
  void doUpdate(Renderer&) override;
  NotificationManager* m_notifications;
  Flex* m_root = nullptr;
  Label* m_status = nullptr;
  std::array<Button*, 4> m_modes{};
};
