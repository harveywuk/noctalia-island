#include "pipewire/pipewire_level_monitor.h"

#include "pipewire/pipewire_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

class PipeWireLevelMonitor::Stream {
public:
  Stream(PipeWireService& service, const std::string& target) {
    if (!service.coreHandle() || target.empty())
      return;
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_NAME, "Noctalia Input Meter",
        PW_KEY_APP_NAME, "Noctalia Input Meter", PW_KEY_STREAM_MONITOR, "true", PW_KEY_TARGET_OBJECT, target.c_str(),
        PW_KEY_NODE_PASSIVE, service.serverSupportsPassiveFollow() ? "in-follow" : "true", "node.dont-reconnect",
        "true", "node.dont-fallback", "true", nullptr
    );
    if (!props)
      return;
    m_stream = pw_stream_new(service.coreHandle(), "noctalia-input-meter", props);
    if (!m_stream) {
      pw_properties_free(props);
      return;
    }
    static const pw_stream_events events = [] {
      pw_stream_events value{};
      value.version = PW_VERSION_STREAM_EVENTS;
      value.destroy = [](void* data) {
        auto& self = *static_cast<Stream*>(data);
        spa_hook_remove(&self.m_listener);
        self.m_stream = nullptr;
        self.m_ready = false;
        self.m_peak = 0;
      };
      value.param_changed = [](void* data, std::uint32_t id, const spa_pod* param) {
        auto& self = *static_cast<Stream*>(data);
        if (id != SPA_PARAM_Format)
          return;
        spa_audio_info_raw format{};
        self.m_ready = param
            && spa_format_audio_raw_parse(param, &format) >= 0
            && format.format == SPA_AUDIO_FORMAT_F32
            && format.channels > 0;
      };
      value.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char*) {
        if (state != PW_STREAM_STATE_STREAMING)
          static_cast<Stream*>(data)->m_peak = 0;
      };
      value.process = [](void* data) { static_cast<Stream*>(data)->process(); };
      return value;
    }();
    pw_stream_add_listener(m_stream, &m_listener, &events, this);
    std::array<std::uint8_t, 512> bytes{};
    auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    auto format = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32);
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0)
      destroy();
  }

  ~Stream() { destroy(); }

  float level() const {
    if (!m_stream || !m_ready || std::chrono::steady_clock::now() - m_received > std::chrono::milliseconds(150))
      return 0;
    // A fixed -60..0 dBFS scale, so quiet input stays quiet rather than being auto-amplified.
    return m_peak > 0 ? std::clamp((20.0F * std::log10(m_peak) + 60.0F) / 60.0F, 0.0F, 1.0F) : 0;
  }

private:
  void destroy() {
    if (!m_stream)
      return;
    spa_hook_remove(&m_listener);
    pw_stream_destroy(m_stream);
    m_stream = nullptr;
  }

  void process() {
    if (!m_stream)
      return;
    auto* buffer = pw_stream_dequeue_buffer(m_stream);
    if (!buffer)
      return;
    float peak = 0;
    if (m_ready && buffer->buffer && buffer->buffer->n_datas > 0) {
      const auto& data = buffer->buffer->datas[0];
      if (data.data && data.chunk && data.chunk->offset <= data.maxsize) {
        const auto count = std::min(data.chunk->size, data.maxsize - data.chunk->offset) / sizeof(float);
        const auto* bytes = static_cast<const std::uint8_t*>(data.data) + data.chunk->offset;
        for (std::size_t i = 0; i < count; ++i) {
          float sample;
          std::memcpy(&sample, bytes + i * sizeof(float), sizeof(sample));
          if (std::isfinite(sample))
            peak = std::max(peak, std::abs(sample));
        }
      }
    }
    m_peak = peak;
    m_received = std::chrono::steady_clock::now();
    pw_stream_queue_buffer(m_stream, buffer);
  }

  pw_stream* m_stream = nullptr;
  spa_hook m_listener{};
  bool m_ready = false;
  float m_peak = 0;
  std::chrono::steady_clock::time_point m_received{};
};

PipeWireLevelMonitor::PipeWireLevelMonitor(PipeWireService& service, const std::string& target)
    : m_stream(std::make_unique<Stream>(service, target)) {}
PipeWireLevelMonitor::~PipeWireLevelMonitor() = default;
float PipeWireLevelMonitor::level() const { return m_stream->level(); }
