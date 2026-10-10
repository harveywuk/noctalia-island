#include "shell/assistant/assistant_audio_level.h"

#include <algorithm>
#include <cmath>
#include <sndfile.h>

float AssistantAudioLevel::amplitude(std::span<const float> samples) {
  if (samples.empty())
    return 0;
  double energy = 0;
  for (const float sample : samples) {
    if (std::isfinite(sample))
      energy += static_cast<double>(sample) * sample;
  }
  const auto rms = static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
  // Gate quiet room noise and compress the speech range without auto-amplifying silence.
  return std::clamp((20.0F * std::log10(std::max(rms, 0.00001F)) + 50.0F) / 40.0F, 0.0F, 1.0F);
}

void AssistantAudioLevel::capture(std::span<const float> samples) {
  m_inputLevel.store(amplitude(samples), std::memory_order_relaxed);
  m_inputAt.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
}

bool AssistantAudioLevel::loadPlayback(const std::filesystem::path& file) {
  SF_INFO info{};
  auto* audio = sf_open(file.c_str(), SFM_READ, &info);
  if (!audio)
    return false;
  if (info.channels != 1
      || info.samplerate < 8000
      || info.samplerate > 192000
      || info.frames <= 0
      || info.frames > static_cast<sf_count_t>(info.samplerate) * 300) {
    sf_close(audio);
    return false;
  }
  // At most 15,000 floats for the entire reply, not another decoded audio copy.
  std::vector<float> samples(static_cast<std::size_t>(info.samplerate / 50));
  std::vector<float> envelope;
  sf_count_t count;
  while ((count = sf_readf_float(audio, samples.data(), static_cast<sf_count_t>(samples.size()))) > 0) {
    envelope.push_back(amplitude(std::span(samples.data(), static_cast<std::size_t>(count))));
    if (envelope.size() > 15000)
      break;
  }
  const bool valid = sf_error(audio) == 0 && !envelope.empty() && envelope.size() <= 15000;
  sf_close(audio);
  if (valid)
    m_envelope = std::move(envelope);
  return valid;
}

void AssistantAudioLevel::playbackEvent(std::string_view chunk) {
  std::scoped_lock lock(m_mutex);
  m_pending.append(chunk);
  for (auto end = m_pending.find('\n'); end != std::string::npos; end = m_pending.find('\n')) {
    const auto line = m_pending.substr(0, end);
    m_pending.erase(0, end + 1);
    if (!line.starts_with("stream state changed ") && line != "stream drained")
      continue;
    const auto now = Clock::now();
    if (line.ends_with(" -> streaming")) {
      if (!m_running) {
        m_started = now;
        m_running = true;
      }
    } else if (m_running) {
      m_elapsed += now - m_started;
      m_running = false;
    }
  }
  if (m_pending.size() > 4096)
    m_pending.clear();
}

float AssistantAudioLevel::level() const {
  if (m_envelope.empty()) {
    const auto updated = Clock::time_point(Clock::duration(m_inputAt.load(std::memory_order_acquire)));
    return Clock::now() - updated < std::chrono::milliseconds(200) ? m_inputLevel.load(std::memory_order_relaxed)
                                                                   : 0.0F;
  }
  std::scoped_lock lock(m_mutex);
  if (!m_running)
    return 0;
  const auto seconds = std::chrono::duration<double>(m_elapsed + (Clock::now() - m_started)).count() - 0.04;
  if (seconds < 0)
    return 0;
  const auto index = static_cast<std::size_t>(seconds * 50.0);
  return index < m_envelope.size() ? m_envelope[index] : 0.0F;
}
