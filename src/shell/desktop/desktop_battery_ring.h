#pragma once

#include "render/scene/node.h"
#include "shell/desktop/desktop_battery_model.h"
#include "ui/signal.h"

class CountdownRingNode;
class Glyph;
class Box;

// Native scene nodes: a muted track, capped charge arc, device icon and charging bolt.
class DesktopBatteryRing final : public Node {
public:
  DesktopBatteryRing();
  void setDevice(const desktop_batteries::Device* device);
  void setDiameter(float size);

private:
  void doLayout(Renderer& renderer) override;
  void applyPalette();
  CountdownRingNode* m_track = nullptr;
  CountdownRingNode* m_arc = nullptr;
  Box* m_startCap = nullptr;
  Box* m_endCap = nullptr;
  Glyph* m_icon = nullptr;
  Box* m_badge = nullptr;
  Glyph* m_bolt = nullptr;
  float m_diameter = 64;
  std::optional<desktop_batteries::Device> m_device;
  Signal<>::ScopedConnection m_palette;
};
