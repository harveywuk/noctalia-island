#pragma once

#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/countdown_ring_node.h"
#include "ui/palette.h"

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
      m_palette = paletteChanged().connect([this] { applyPalette(); });
      applyPalette();
      setVisible(false);
    }

    void setGeometry(float width, float height, float radius, float scale) {
      setSize(width, height);
      for (auto* ring : {m_track, m_fill}) {
        ring->setSize(width, height);
        ring->setCornerRadius(radius);
        ring->setThickness(2.5F * scale);
      }
    }

    void update(bool active, std::optional<float> progress, ColorRole role, bool charging = false) {
      setVisible(active);
      m_indeterminate = !progress;
      m_charging = charging;
      if (role != m_role) {
        m_role = role;
        applyPalette();
      }
      m_fill->setProgress(progress.value_or(0.22F));
      const bool animate = active && (m_indeterminate || charging) && MotionService::instance().enabled();
      if (!animate && m_animation) {
        animationManager()->cancel(m_animation);
        m_animation = 0;
      }
      if (!m_indeterminate)
        m_fill->setStartOffset(0);
      if (!charging)
        m_fill->setOpacity(1);
      if (animate && !m_animation)
        loop();
    }

  private:
    void loop() {
      if (!animationManager() || !visible() || !MotionService::instance().enabled()) {
        m_fill->setOpacity(1);
        return;
      }
      m_animation = animationManager()->animate(
          0, 1, 1800, Easing::Linear,
          [this](float t) {
            if (m_indeterminate)
              m_fill->setStartOffset(t);
            m_fill->setOpacity(m_charging ? 0.72F + 0.28F * std::cos(t * 6.2831853F) : 1.0F);
          },
          [this] {
            m_animation = 0;
            loop();
          },
          this
      );
    }
    void applyPalette() {
      m_track->setColor(resolveColorSpec(colorSpecFromRole(ColorRole::SurfaceVariant)));
      m_fill->setColor(resolveColorSpec(colorSpecFromRole(m_role)));
    }
    CountdownRingNode* m_track = nullptr;
    CountdownRingNode* m_fill = nullptr;
    ColorRole m_role = ColorRole::Primary;
    bool m_indeterminate = false;
    bool m_charging = false;
    AnimationManager::Id m_animation = 0;
    Signal<>::ScopedConnection m_palette;
  };
} // namespace island
