#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <map>

class PipeWireService;
class MprisService;

// Temporarily lowers only playback streams belonging to playing media players.
// External volume changes relinquish ownership, so restoration cannot undo them.
class AssistantDucking {
public:
  AssistantDucking(PipeWireService* audio, MprisService* media) : m_audio(audio), m_media(media) {}
  ~AssistantDucking();
  void setActive(bool active);

private:
  struct Stream {
    std::uint64_t serial;
    float original, written, start, target;
    std::chrono::steady_clock::time_point since;
  };
  void tick(bool immediate = false);
  PipeWireService* m_audio;
  MprisService* m_media;
  std::map<std::uint32_t, Stream> m_streams;
  Timer m_timer;
  bool m_active = false;
};
