#pragma once

#include "render/scene/countdown_ring_node.h"

#include <array>
#include <cmath>

namespace island {
  // A moving identity gradient, feathered outwards into a soft halo.
  class PanelAccent final : public Node {
  public:
    PanelAccent() {
      setHitTestVisible(false);
      for (std::size_t i = 0; i < m_rings.size(); ++i) {
        m_rings[i] = static_cast<CountdownRingNode*>(addChild(std::make_unique<CountdownRingNode>()));
        m_rings[i]->setColor(rgba(1, 1, 1));
        m_rings[i]->setProgress(1);
        m_rings[i]->setOpacity(kWeights[i]);
      }
    }

    void setGeometry(float x, float y, float width, float height, float radius, float scale) {
      const float spread = kBand * static_cast<float>(m_rings.size()) * scale;
      setPosition(x - spread, y - spread);
      setSize(width + spread * 2, height + spread * 2);
      for (std::size_t i = 0; i < m_rings.size(); ++i) {
        const float outset = kBand * static_cast<float>(i + 1) * scale;
        m_rings[i]->setPosition(spread - outset, spread - outset);
        m_rings[i]->setSize(width + 2 * outset, height + 2 * outset);
        m_rings[i]->setCornerRadius(radius + outset);
        m_rings[i]->setThickness(kBand * scale);
      }
    }

    void advance(float deltaMs, const std::array<Color, 4>& palette, bool motion) {
      m_phase = motion ? std::fmod(m_phase + std::clamp(deltaMs, 0.0F, 100.0F) / 18000.0F, 1.0F) : 0;
      RingColors colors;
      for (std::size_t i = 0; i < colors.size(); ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(colors.size() - 1) * 4;
        const auto index = static_cast<std::size_t>(position);
        const float t = position - static_cast<float>(index);
        colors[i] = lerpColor(palette[index % 4], palette[(index + 1) % 4], t * t * (3 - 2 * t));
      }
      for (auto* ring : m_rings) {
        ring->setColors(colors);
        // Follow the palette cycle's direction so the two motions reinforce each other.
        ring->setStartOffset(-m_phase);
      }
    }

  private:
    static constexpr float kBand = 2.0F;
    static constexpr std::array<float, 6> kWeights{0.72F, 0.46F, 0.28F, 0.15F, 0.07F, 0.02F};
    std::array<CountdownRingNode*, kWeights.size()> m_rings{};
    float m_phase = 0;
  };
} // namespace island
