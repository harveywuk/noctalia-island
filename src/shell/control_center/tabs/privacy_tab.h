#pragma once

#include "shell/control_center/tab.h"
#include "shell/island/island_privacy.h"

class ConfigService;
class CompositorPlatform;

class PrivacyTab final : public Tab {
public:
  PrivacyTab(PipeWireService* audio, ConfigService* config, CompositorPlatform* platform)
      : m_audio(audio), m_config(config), m_platform(platform) {}
  std::unique_ptr<Flex> create() override;
  void onClose() override;

private:
  bool rebuild();
  void openApp(PrivacyCaptureKind kind, const std::string& app);
  void doLayout(Renderer&, float width, float height) override;
  void doUpdate(Renderer&) override;
  PipeWireService* m_audio;
  ConfigService* m_config;
  CompositorPlatform* m_platform;
  Flex* m_root = nullptr;
  Flex* m_list = nullptr;
  Label* m_feedback = nullptr;
  island::PrivacySummary m_summary;
  PrivacyState m_state;
  ShellConfig::PrivacyConfig m_filters;
  bool m_built = false;
};
