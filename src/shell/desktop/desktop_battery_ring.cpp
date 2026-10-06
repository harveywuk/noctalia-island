#include "shell/desktop/desktop_battery_ring.h"

#include "render/scene/countdown_ring_node.h"
#include "ui/builders.h"

#include <cmath>
#include <numbers>

DesktopBatteryRing::DesktopBatteryRing() {
  setHitTestVisible(false);
  auto track = std::make_unique<CountdownRingNode>();
  m_track = static_cast<CountdownRingNode*>(addChild(std::move(track)));
  auto arc = std::make_unique<CountdownRingNode>();
  m_arc = static_cast<CountdownRingNode*>(addChild(std::move(arc)));
  addChild(ui::box({.out = &m_startCap}));
  addChild(ui::box({.out = &m_endCap}));
  addChild(ui::glyph({.out = &m_icon}));
  addChild(ui::box({.out = &m_badge}));
  addChild(ui::glyph({.out = &m_bolt, .glyph = "bolt-filled"}));
  for (const auto& child : children())
    child->setHitTestVisible(false);
  m_palette = paletteChanged().connect([this]() { applyPalette(); });
  applyPalette();
}

void DesktopBatteryRing::setDevice(const desktop_batteries::Device* device) {
  const auto next = device ? std::optional{*device} : std::nullopt;
  if (next == m_device)
    return;
  m_device = next;
  applyPalette();
  markLayoutDirty();
}
void DesktopBatteryRing::setDiameter(float size) {
  m_diameter = std::max(1.0F, size);
  setSize(m_diameter, m_diameter);
}
void DesktopBatteryRing::applyPalette() {
  const auto role = m_device && desktop_batteries::low(*m_device) ? ColorRole::Error : ColorRole::Primary;
  const auto color = colorSpecFromRole(role);
  m_track->setColor(resolveColorSpec(colorSpecFromRole(ColorRole::OnSurfaceVariant, m_device ? 0.18F : 0.10F)));
  m_arc->setColor(resolveColorSpec(color));
  m_startCap->setFill(color);
  m_endCap->setFill(color);
  m_icon->setColor(colorSpecFromRole(ColorRole::OnSurface));
  m_badge->setFill(colorSpecFromRole(ColorRole::Surface));
  m_bolt->setColor(color);
}
void DesktopBatteryRing::doLayout(Renderer& renderer) {
  const float thickness = std::max(2.0F, m_diameter * 0.075F);
  const float progress = m_device && m_device->percentage ? *m_device->percentage / 100 : 0;
  const float radius = (m_diameter - thickness) * 0.5F;
  for (auto* ring : {m_track, m_arc}) {
    ring->setSize(m_diameter, m_diameter);
    ring->setThickness(thickness);
    ring->setCornerRadius(m_diameter * 0.5F);
  }
  m_track->setProgress(1);
  m_arc->setProgress(progress);
  m_arc->setVisible(progress > 0);
  for (auto* cap : {m_startCap, m_endCap}) {
    cap->setVisible(progress > 0 && progress < 1);
    cap->setSize(thickness, thickness);
    cap->setRadius(thickness * 0.5F);
  }
  m_startCap->setPosition(radius, 0);
  const float angle = progress * 2 * std::numbers::pi_v<float> - std::numbers::pi_v<float> * 0.5F;
  m_endCap->setPosition(radius + radius * std::cos(angle), radius + radius * std::sin(angle));
  m_icon->setVisible(m_device.has_value());
  if (m_device) {
    m_icon->setGlyph(m_device->glyph);
    m_icon->setGlyphSize(m_diameter * 0.43F);
    m_icon->layout(renderer);
    m_icon->setPosition((m_diameter - m_icon->width()) * .5F, (m_diameter - m_icon->height()) * .5F);
  }
  const bool charging = m_device && desktop_batteries::charging(*m_device);
  m_badge->setVisible(charging);
  m_bolt->setVisible(charging);
  if (charging) {
    const float badge = m_diameter * .25F;
    m_badge->setSize(badge, badge);
    m_badge->setRadius(badge * .5F);
    m_badge->setPosition((m_diameter - badge) * .5F, -badge * .3F);
    m_bolt->setGlyphSize(badge * .9F);
    m_bolt->layout(renderer);
    m_bolt->setPosition((m_diameter - m_bolt->width()) * .5F, -badge * .3F + (badge - m_bolt->height()) * .5F);
  }
}
