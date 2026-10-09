#pragma once

#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/countdown_ring_node.h"
#include "ui/palette.h"

#include <array>
#include <chrono>
#include <cmath>
#include <optional>

namespace island {
  // A slow pulse outside the capsule: purple for desktop sharing, red for recording starts and alerts;
  // green for completed transfers, or charge colour for a newly connected battery.
  // It sits outside the progress outline and ignores input.
  class CaptureGlow final : public Node {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr float kPeriodMs = 2400.0F;
    static constexpr auto kRecordingIntro = std::chrono::milliseconds(4800);
    // Concentric 2 px bands outward from the capsule edge, each fainter than the last, so the
    // glow fades into the background instead of ending at a hard line.
    static constexpr float kBand = 2.0F;
    static constexpr std::array<float, 6> kWeights{1.0F, 0.7F, 0.45F, 0.26F, 0.12F, 0.04F};
    static constexpr float kOutset = kBand * kWeights.size();
    static ColorSpec sharingColor() { return fixedColorSpec(rgba(0.749F, 0.353F, 0.949F)); }

    CaptureGlow() {
      setHitTestVisible(false);
      for (auto& ring : m_rings) {
        ring = static_cast<CountdownRingNode*>(addChild(std::make_unique<CountdownRingNode>()));
        ring->setProgress(1.0F);
      }
      m_palette = paletteChanged().connect([this] { applyPalette(); });
      setVisible(false);
    }

    // Capsule geometry; the glow spreads kOutset logical pixels outside it.
    void setGeometry(float x, float y, float width, float height, float radius, float scale) {
      const float outset = kOutset * scale;
      setPosition(x - outset, y - outset);
      setSize(width + 2 * outset, height + 2 * outset);
      for (std::size_t i = 0; i < m_rings.size(); ++i) {
        // A ring strokes inside its bounds, so band i covers (i, i + 1] bands out from the edge.
        const float ringOutset = kBand * static_cast<float>(i + 1) * scale;
        m_rings[i]->setPosition(outset - ringOutset, outset - ringOutset);
        m_rings[i]->setSize(width + 2 * ringOutset, height + 2 * ringOutset);
        m_rings[i]->setCornerRadius(radius + ringOutset);
        m_rings[i]->setThickness(kBand * scale);
      }
    }

    void update(
        bool active, ColorSpec color, bool persistent = false,
        std::optional<Clock::time_point> recordingStarted = std::nullopt
    ) {
      if (!(color == m_color)) {
        m_color = color;
        applyPalette();
      }
      if (persistent != m_persistent) {
        m_persistent = persistent;
        setStrength(m_strength);
      }
      if (active == m_active && recordingStarted == m_recordingStarted)
        return;
      m_active = active;
      m_recordingStarted = recordingStarted;
      setVisible(active);
      if (m_animation) {
        if (animationManager())
          animationManager()->cancel(m_animation);
        m_animation = 0;
      }
      if (active) {
        if (recordingStarted) {
          recordingIntro(*recordingStarted);
          return;
        }
        setStrength(0.5F);
        loop();
      }
    }

  private:
    void recordingIntro(Clock::time_point started) {
      // Use the recorder's real start time across monitors, view changes and reloads.
      // A real-time driver also lets reduced motion expire without playing an animation.
      const float elapsed = std::chrono::duration<float, std::milli>(Clock::now() - started).count();
      const float duration = static_cast<float>(kRecordingIntro.count());
      if (elapsed >= duration) {
        setVisible(false);
        return;
      }
      const auto strength = [this](float age) {
        setStrength(
            MotionService::instance().enabled() ? 0.35F * (1.0F - std::cos(age / kPeriodMs * 6.2831853F)) : 0.35F
        );
      };
      strength(elapsed);
      if (!animationManager())
        return;
      m_animation = animationManager()->animateTimer(
          elapsed, duration, duration - elapsed, Easing::Linear, strength,
          [this] {
            m_animation = 0;
            setVisible(false);
          },
          this
      );
    }
    void loop() {
      // With reduced motion the glow holds at mid strength instead of pulsing.
      if (!m_active || !animationManager() || !MotionService::instance().enabled())
        return;
      m_animation = animationManager()->animate(
          0, 1, kPeriodMs, Easing::Linear,
          [this](float t) {
            setStrength(MotionService::instance().enabled() ? 0.5F - 0.5F * std::cos(t * 6.2831853F) : 0.5F);
          },
          [this] {
            m_animation = 0;
            loop();
          },
          this
      );
    }
    void setStrength(float strength) {
      m_strength = strength;
      // Sharing must stay visible through the trough of every pulse (22% to 55% at the edge).
      if (m_persistent)
        strength = 0.4F + 0.6F * strength;
      for (std::size_t i = 0; i < m_rings.size(); ++i)
        m_rings[i]->setOpacity(0.55F * strength * kWeights[i]);
    }
    void applyPalette() {
      const Color color = resolveColorSpec(m_color);
      for (auto* ring : m_rings)
        ring->setColor(color);
    }
    std::array<CountdownRingNode*, kWeights.size()> m_rings{};
    ColorSpec m_color = colorSpecFromRole(ColorRole::Error);
    bool m_active = false;
    bool m_persistent = false;
    std::optional<Clock::time_point> m_recordingStarted;
    float m_strength = 0.5F;
    AnimationManager::Id m_animation = 0;
    Signal<>::ScopedConnection m_palette;
  };
} // namespace island
