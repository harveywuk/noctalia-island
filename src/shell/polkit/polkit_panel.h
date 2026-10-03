#pragma once

#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>
#include <string_view>

class Button;
class Flex;
class Glyph;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class PolkitAgent;
class PolkitRequest;
class Renderer;
class ConfigService;

// The password prompt, laid out like the macOS Ventura authentication alert: a centred app icon
// with a padlock badge, a bold title, a short hint, the password field, and two
// equal-width buttons. A wrong password shakes the alert.
class PolkitPanel : public Panel {
public:
  PolkitPanel(ConfigService* config, std::function<PolkitAgent*()> agentProvider);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(kAlertWidth + Style::panelPadding * 2.0F); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  static constexpr float kAlertWidth = 300.0F;
  static constexpr float kIconSize = 64.0F;
  static constexpr float kBadgeSize = 26.0F;

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit(std::string_view response = {});
  void cancelAuth();
  bool handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void resolveIcon(Renderer& renderer, const PolkitRequest& request);
  void startShake();
  void applyShake();

  ConfigService* m_config = nullptr;
  std::function<PolkitAgent*()> m_agentProvider;
  Flex* m_rootLayout = nullptr;
  Flex* m_content = nullptr;
  Flex* m_fields = nullptr;
  InputArea* m_focusArea = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_promptLabel = nullptr;
  Label* m_supplementaryLabel = nullptr;
  Input* m_input = nullptr;
  Button* m_submitButton = nullptr;
  Button* m_cancelButton = nullptr;
  Node* m_iconContainer = nullptr;
  Flex* m_iconWell = nullptr;
  Flex* m_lockBadge = nullptr;
  Image* m_icon = nullptr;
  Glyph* m_fallbackIcon = nullptr;
  IconResolver m_iconResolver;
  std::string m_lastIconName;
  bool m_iconResolved = false;
  bool m_lastResponseRequired = false;
  bool m_lastInvalidPassword = false;
  std::string m_trackedRequestCookie;
  bool m_hasTrackedRequest = false;
  // Natural height of the alert from the last layout, so the panel fits it exactly.
  float m_measuredHeight = 0.0F;
  float m_contentBaseX = 0.0F;
  float m_shakeOffset = 0.0F;
  std::uint32_t m_shakeAnimId = 0;
};
