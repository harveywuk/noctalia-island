#pragma once

#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/countdown_ring_node.h"
#include "ui/palette.h"

#include <array>
#include <cmath>
#include <optional>

namespace island {
  // A decorative sibling of the capsule, so its stroke follows animated geometry
  // without clipping content or participating in pointer hit testing.
  class ProgressOutline final : public Node {
  public:
    ProgressOutline() {
      setHitTestVisible(false);
      auto track = std::make_unique<CountdownRingNode>();
      m_track = static_cast<CountdownRingNode*>(addChild(std::move(track)));
      auto fill = std::make_unique<CountdownRingNode>();
      m_fill = static_cast<CountdownRingNode*>(addChild(std::move(fill)));
      for (auto& glow : m_glow) {
        glow = static_cast<CountdownRingNode*>(addChild(std::make_unique<CountdownRingNode>()));
        glow->setZIndex(-1);
      }
      m_palette = paletteChanged().connect([this] { applyPalette(); });
      applyPalette();
      setVisible(false);
    }

    void setGeometry(float width, float height, float radius, float scale) {
      setSize(width, height);
      for (auto* ring : {m_track, m_fill}) {
        ring->setSize(width, height);
        ring->setCornerRadius(radius);
        ring->setThickness(kStroke * scale);
      }
      for (std::size_t i = 0; i < m_glow.size(); ++i) {
        const float spread = 1.5F * static_cast<float>(i + 1) * scale;
        auto* glow = m_glow[i];
        glow->setPosition(-spread, -spread);
        glow->setSize(width + 2 * spread, height + 2 * spread);
        glow->setCornerRadius(radius + spread);
        // A wider stroke around the same centre line keeps the halo's progress
        // and rounded corners aligned with the fine foreground stroke.
        glow->setThickness(kStroke * scale + 2 * spread);
      }
    }

    // fill/track are colour specs so the Cupertino appearance can pass fixed activity tints.
    void update(
        bool active, std::optional<float> progress, ColorSpec fill, bool charging = false,
        ColorSpec track = colorSpecFromRole(ColorRole::OnSurface, 0.16F),
        const std::optional<RingColors>& colors = std::nullopt
    ) {
      setVisible(active);
      m_indeterminate = !progress && !colors;
      m_charging = charging;
      if (!(fill == m_fillSpec) || !(track == m_trackSpec)) {
        m_fillSpec = fill;
        m_trackSpec = track;
        applyPalette();
      }
      m_fill->setProgress(progress.value_or(0.22F));
      m_fill->setColors(colors);
      for (auto* glow : m_glow) {
        glow->setProgress(progress.value_or(0.22F));
        glow->setColors(colors);
      }
      const bool animate = active && (m_indeterminate || charging) && MotionService::instance().enabled();
      if (!animate && m_animation) {
        animationManager()->cancel(m_animation);
        m_animation = 0;
      }
      if (!m_indeterminate)
        setStartOffset(0);
      if (!charging)
        setStrength(1);
      if (animate && !m_animation)
        loop();
    }

  private:
    void loop() {
      if (!animationManager() || !visible() || !MotionService::instance().enabled()) {
        setStrength(1);
        return;
      }
      m_animation = animationManager()->animate(
          0, 1, 1800, Easing::Linear,
          [this](float t) {
            if (m_indeterminate)
              setStartOffset(t);
            setStrength(m_charging ? 0.72F + 0.28F * std::cos(t * 6.2831853F) : 1.0F);
          },
          [this] {
            m_animation = 0;
            loop();
          },
          this
      );
    }
    void setStartOffset(float offset) {
      m_fill->setStartOffset(offset);
      for (auto* glow : m_glow)
        glow->setStartOffset(offset);
    }
    void setStrength(float strength) {
      m_fill->setOpacity(strength);
      for (auto* glow : m_glow)
        glow->setOpacity(strength);
    }
    void applyPalette() {
      auto track = resolveColorSpec(m_trackSpec);
      track.a *= 0.5F;
      m_track->setColor(track);
      const auto tint = resolveColorSpec(m_fillSpec);
      for (std::size_t i = 0; i < m_glow.size(); ++i) {
        auto glow = tint;
        glow.a *= kGlowAlpha[i];
        m_glow[i]->setColor(glow);
      }
      auto fill = tint;
      fill.a *= 0.8F;
      m_fill->setColor(fill);
    }
    static constexpr float kStroke = 1.5F;
    static constexpr std::array<float, 3> kGlowAlpha{0.12F, 0.06F, 0.03F};
    CountdownRingNode* m_track = nullptr;
    CountdownRingNode* m_fill = nullptr;
    std::array<CountdownRingNode*, kGlowAlpha.size()> m_glow{};
    ColorSpec m_fillSpec = colorSpecFromRole(ColorRole::Primary);
    ColorSpec m_trackSpec = colorSpecFromRole(ColorRole::OnSurface, 0.16F);
    bool m_indeterminate = false;
    bool m_charging = false;
    AnimationManager::Id m_animation = 0;
    Signal<>::ScopedConnection m_palette;
  };
} // namespace island
