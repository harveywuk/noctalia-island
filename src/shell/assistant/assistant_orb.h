#pragma once

#include "render/scene/effect_node.h"

class AssistantOrb final : public EffectNode {
public:
  enum class State { Idle, Listening, Thinking, Speaking, Complete, Error };
  AssistantOrb();
  bool setState(State state);
  void animateEyes();
  // Returns whether an expression, blink or brief idle gesture needs another frame.
  bool tick(float deltaMs, float energy, bool motion);

private:
  static OrbFaceStyle expression(State state);
  State m_state = State::Idle;
  OrbFaceStyle m_from;
  float m_transitionMs = 600;
  float m_blinkMs = 300;
  float m_idleMs = 2200;
  float m_time = 0;
  float m_colorPhase = 0;
  unsigned m_gesture = 0;
  unsigned m_blinks = 0;
  bool m_doubleBlink = false;
};
