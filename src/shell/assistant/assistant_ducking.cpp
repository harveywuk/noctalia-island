#include "shell/assistant/assistant_ducking.h"

#include "dbus/mpris/mpris_service.h"
#include "pipewire/pipewire_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>

namespace {
  std::string identity(std::string value) {
    value = StringUtils::toLower(value);
    if (value.ends_with(".desktop"))
      value.resize(value.size() - 8);
    return value;
  }
} // namespace

AssistantDucking::~AssistantDucking() {
  setActive(false);
  tick(true);
}

void AssistantDucking::setActive(bool active) {
  if (active == m_active)
    return;
  m_active = active;
  const auto now = std::chrono::steady_clock::now();
  if (active && m_audio && m_media) {
    // Interrupt a restore smoothly, retaining the original volume across voice phases.
    for (auto& [id, stream] : m_streams) {
      stream.start = stream.written;
      stream.target = stream.original * .45F;
      stream.since = now;
    }
    const auto streams = m_audio->state().programOutputs;
    for (const auto& stream : streams) {
      if (stream.muted || stream.volume <= 0 || !stream.serial)
        continue;
      for (const auto& [bus, player] : m_media->players()) {
        const auto id = identity(player.desktopEntry);
        if (player.playbackStatus != "Playing" || id.empty())
          continue;
        if (id != identity(stream.applicationId) && id != identity(stream.applicationBinary))
          continue;
        m_streams.emplace(
            stream.id, Stream{stream.serial, stream.volume, stream.volume, stream.volume, stream.volume * .45F, now}
        );
        break;
      }
    }
  } else {
    for (auto& [id, stream] : m_streams) {
      stream.start = stream.written;
      stream.target = stream.original;
      stream.since = now;
    }
  }
  if (!m_streams.empty())
    m_timer.startRepeating(std::chrono::milliseconds(40), [this] { tick(); });
}

void AssistantDucking::tick(bool immediate) {
  if (!m_audio)
    return;
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_streams.begin(); it != m_streams.end();) {
    auto& entry = it->second;
    const auto& outputs = m_audio->state().programOutputs;
    const auto live = std::ranges::find(outputs, it->first, &AudioNode::id);
    if (live == outputs.end() || live->serial != entry.serial || std::abs(live->volume - entry.written) > .005F) {
      it = m_streams.erase(it);
      continue;
    }
    const float progress = immediate
        ? 1.0F
        : std::clamp(std::chrono::duration<float, std::milli>(now - entry.since).count() / 240.0F, 0.0F, 1.0F);
    const float eased = progress * progress * (3.0F - 2.0F * progress);
    const float next = entry.start + (entry.target - entry.start) * eased;
    if (std::abs(next - entry.written) > .0001F) {
      entry.written = next;
      m_audio->setProgramOutputVolumeTemporary(it->first, entry.written);
    }
    if (progress == 1 && (!m_active || immediate))
      it = m_streams.erase(it);
    else
      ++it;
  }
  if (m_streams.empty())
    m_timer.stop();
}
