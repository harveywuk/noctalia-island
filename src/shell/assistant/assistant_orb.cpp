#include "shell/assistant/assistant_orb.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
  float ease(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
  }
  float mix(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

OrbFaceStyle AssistantOrb::expression(State state) {
  OrbFaceStyle face;
  // Every state keeps a multicolour interior; only the emphasis changes.
  face.colors = {hex("#47d8ff"), hex("#617cff"), hex("#c075ff"), hex("#ff91bc")};
  switch (state) {
  case State::Idle:
    break;
  case State::Listening:
    face.colors = {hex("#48f0dc"), hex("#439aff"), hex("#9f87ff"), hex("#edacff")};
    face.eyeOpen = 1.25F;
    face.smile = 0.35F;
    break;
  case State::Thinking:
    face.colors = {hex("#7194ff"), hex("#9470ff"), hex("#dc77ef"), hex("#fda5d0")};
    face.eyeOpen = 0.65F;
    face.gazeX = -0.08F;
    face.gazeY = -0.035F;
    face.smile = -0.12F;
    break;
  case State::Speaking:
    face.colors = {hex("#58dcff"), hex("#8684ff"), hex("#f287d6"), hex("#ffb394")};
    face.eyeOpen = 0.86F;
    face.smile = 0.4F;
    break;
  case State::Complete:
    face.colors = {hex("#65edcc"), hex("#59cafa"), hex("#a297ff"), hex("#e1b5fa")};
    face.eyeOpen = 0.8F;
    face.smile = 1;
    break;
  case State::Error:
    face.colors = {hex("#ffc482"), hex("#d28ccf"), hex("#9b85ef"), hex("#84b9f5")};
    face.eyeOpen = 0.85F;
    face.smile = -0.55F;
    face.puzzled = 1;
    break;
  }
  return face;
}

AssistantOrb::AssistantOrb() {
  setEffectType(EffectType::AssistantOrb);
  setBgColor(hex("#ffffff"));
  setHitTestVisible(false);
  m_from = expression(State::Idle);
  setOrbFace(m_from);
}

bool AssistantOrb::setState(State state) {
  if (m_state == state)
    return false;
  m_from = style().orbFace;
  m_state = state;
  m_transitionMs = 0;
  m_idleMs = 2200;
  return true;
}

void AssistantOrb::animateEyes() {
  if (m_state == State::Error)
    return;
  m_blinkMs = 0;
  m_doubleBlink = ++m_blinks % 3 == 2;
  if (m_state == State::Idle || m_state == State::Complete) {
    m_idleMs = 0;
    ++m_gesture;
  }
}

bool AssistantOrb::tick(float deltaMs, float energy, bool motion) {
  const float dt = std::clamp(deltaMs, 0.0F, 100.0F);
  m_transitionMs = motion ? std::min(600.0F, m_transitionMs + dt) : 600.0F;
  const float blinkEnd = m_doubleBlink ? 585.0F : 245.0F;
  m_blinkMs = motion ? std::min(blinkEnd, m_blinkMs + dt) : blinkEnd;
  m_idleMs = motion ? std::min(2200.0F, m_idleMs + dt) : 2200.0F;
  auto face = expression(m_state);
  if (motion && (m_state == State::Idle || m_state == State::Complete)) {
    m_colorPhase = std::fmod(m_colorPhase + dt / 20000.0F, 1.0F);
    const auto colors = face.colors;
    const float offset = m_colorPhase * static_cast<float>(colors.size());
    const auto step = static_cast<std::size_t>(offset);
    const float blend = ease(offset - static_cast<float>(step));
    for (std::size_t i = 0; i < colors.size(); ++i)
      face.colors[i] = lerpColor(colors[(i + step) % colors.size()], colors[(i + step + 1) % colors.size()], blend);
  }
  const float colorMix = ease(m_transitionMs / 600.0F);
  const float shapeMix = ease(m_transitionMs / 180.0F);
  for (std::size_t i = 0; i < face.colors.size(); ++i)
    face.colors[i] = lerpColor(m_from.colors[i], face.colors[i], colorMix);
  face.eyeOpen = mix(m_from.eyeOpen, face.eyeOpen, shapeMix);
  face.gazeX = mix(m_from.gazeX, face.gazeX, shapeMix);
  face.gazeY = mix(m_from.gazeY, face.gazeY, shapeMix);
  face.smile = mix(m_from.smile, face.smile, shapeMix);
  face.puzzled = mix(m_from.puzzled, face.puzzled, ease(m_transitionMs / 420.0F));
  face.energy = motion ? std::clamp(energy, 0.0F, 1.0F) : 0.0F;
  if (m_state == State::Speaking)
    face.eyeOpen += face.energy * 0.18F;
  const float blinkMs = m_doubleBlink && m_blinkMs >= 340 ? m_blinkMs - 340 : m_blinkMs;
  if (blinkMs < 50)
    face.blink = 1.0F - ease(blinkMs / 50.0F);
  else if (blinkMs < 95)
    face.blink = 0;
  else
    face.blink = ease((blinkMs - 95) / 150.0F);

  // Eye gestures pause between appearances while the ambient smoke keeps drifting.
  float breath = 0;
  float tilt = 0;
  if (m_idleMs < 2200) {
    const float look = ease(m_idleMs / 350.0F) * (1.0F - ease((m_idleMs - 1150.0F) / 650.0F));
    const unsigned gesture = (m_gesture - 1) % 4;
    const float direction = gesture == 0 ? -1.0F : gesture == 2 ? 1.0F : 0.0F;
    face.gazeX += direction * 0.11F * look;
    face.gazeY += (gesture == 1 ? -0.10F : 0.025F) * look;
    face.eyeOpen += (gesture == 1 ? 0.15F : gesture == 3 ? -0.12F : 0.03F) * look;
    const float wave = std::sin(std::numbers::pi_v<float> * m_idleMs / 2200.0F);
    breath = wave * wave;
    tilt = direction * 0.035F * breath;
  }
  setScale(1.0F + breath * 0.02F, 1.0F - breath * 0.012F);
  setRotation(tilt);
  const float speed = m_state == State::Thinking ? 0.001F : 0.0005F + face.energy * 0.0015F;
  if (motion && speed > 0)
    m_time = std::fmod(m_time + dt * speed, 3600.0F);
  if (motion && m_state == State::Thinking)
    face.gazeX += 0.025F * std::sin(m_time * 1.5F);
  setTime(motion ? m_time : 0);
  setOrbFace(face);
  return m_transitionMs < 600 || m_blinkMs < blinkEnd || m_idleMs < 2200;
}
