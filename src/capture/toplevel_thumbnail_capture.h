#pragma once

#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

class WaylandConnection;
struct ToplevelThumbnailCapturePending;
struct ext_foreign_toplevel_handle_v1;

namespace capture {
  [[nodiscard]] std::optional<ScreencopyImage> makeToplevelThumbnail(
      std::span<const std::uint8_t> pixels, int width, int height, std::uint32_t format, int maxWidth, int maxHeight,
      std::int32_t transform
  );
}

// Bounded-size snapshots or a throttled stream from an ext-foreign-toplevel handle.
class ToplevelThumbnailCapture {
public:
  using CompletionCallback = std::function<void(std::optional<ScreencopyImage>, std::string error)>;

  explicit ToplevelThumbnailCapture(WaylandConnection& wayland);
  ~ToplevelThumbnailCapture();

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] bool busy() const noexcept { return m_pending != nullptr; }
  void capture(ext_foreign_toplevel_handle_v1* handle, int maxWidth, int maxHeight, CompletionCallback onComplete);
  // Reuses the session/buffer until cancellation. After the first frame, an
  // unchanged source may wait indefinitely without waking the client.
  void stream(
      ext_foreign_toplevel_handle_v1* handle, int maxWidth, int maxHeight, std::chrono::milliseconds interval,
      CompletionCallback onFrame
  );
  void cancelInFlight();

private:
  friend struct ToplevelThumbnailCapturePending;
  void fail(std::string message);
  void finish(ScreencopyImage image);
  void startCapture(
      ext_foreign_toplevel_handle_v1* handle, int maxWidth, int maxHeight, std::chrono::milliseconds interval,
      CompletionCallback onComplete
  );
  void scheduleFrame();

  WaylandConnection& m_wayland;
  std::unique_ptr<ToplevelThumbnailCapturePending> m_pending;
  CompletionCallback m_onComplete;
  Timer m_timeout;
  Timer m_refresh;
  std::chrono::milliseconds m_interval{0};
};
