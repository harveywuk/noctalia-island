#pragma once

#include "shell/control_center/tab.h"

#include <array>

class ConfigService;
class NotificationManager;
class Button;
class Input;
class Select;
class Toggle;

class FocusTab final : public Tab {
public:
  FocusTab(NotificationManager* notifications, ConfigService* config)
      : m_notifications(notifications), m_config(config) {}
  std::unique_ptr<Flex> create() override;
  void onClose() override;

private:
  void loadProfile(std::size_t index);
  void saveProfile();
  void syncStatus();
  void doLayout(Renderer&, float width, float height) override;
  void doUpdate(Renderer&) override;
  NotificationManager* m_notifications;
  ConfigService* m_config;
  Flex* m_root = nullptr;
  Label* m_status = nullptr;
  Label* m_feedback = nullptr;
  Input* m_apps = nullptr;
  Input* m_start = nullptr;
  Input* m_end = nullptr;
  Toggle* m_schedule = nullptr;
  Toggle* m_critical = nullptr;
  Toggle* m_recording = nullptr;
  std::array<Button*, 4> m_modes{};
  std::array<Button*, 7> m_days{};
  int m_dayMask = 31;
  std::size_t m_editing = 0;
};
