#include "capture/screenshot_service.h"

#include "capture/screen_recorder.h"
#include "capture/screenshot_region_overlay.h"
#include "compositors/compositor_platform.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "launcher/launcher_util.h"
#include "notification/notification.h"
#include "notification/notification_manager.h"
#include "pipewire/sound_player.h"
#include "render/core/image_encoder.h"
#include "render/core/image_file_loader.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stb/stb_image_resize2.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("screenshot");
  // Wide enough for the notification's thumbnail at 2x scale.
  constexpr int kScreenshotThumbnailWidth = 480;
  constexpr const char* kScreenshotPathEnv = "NOCTALIA_SCREENSHOT_PATH";
  constexpr const char* kStateOwner = "screenshot";
  constexpr const char* kLastRegionKey = "last_region";
  constexpr const char* kAnnotateStateOwner = "annotate";

  [[nodiscard]] std::optional<double> parseDouble(std::string_view text) {
    double value = 0.0;
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] std::optional<capture::AnnotationColor> parseAnnotationColor(std::string_view text) {
    std::array<float, 4> channels{};
    std::size_t index = 0;
    std::size_t start = 0;
    while (index < channels.size()) {
      const std::size_t comma = text.find(',', start);
      const std::string_view field = text.substr(start, comma == std::string_view::npos ? comma : comma - start);
      const auto parsed = parseDouble(field);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      channels[index] = std::clamp(static_cast<float>(*parsed), 0.0F, 1.0F);
      ++index;
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }
    if (index != channels.size()) {
      return std::nullopt;
    }
    return capture::AnnotationColor{
        .r = channels[0],
        .g = channels[1],
        .b = channels[2],
        .a = channels[3],
    };
  }

  [[nodiscard]] std::string encodeRegion(const LogicalRect& region) {
    return std::format("{},{},{},{}", region.x, region.y, region.width, region.height);
  }

  [[nodiscard]] std::optional<LogicalRect> parseRegion(std::string_view text) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    const std::string copy(text);
    if (std::sscanf(copy.c_str(), "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
      return std::nullopt;
    }
    if (width < 2 || height < 2) {
      return std::nullopt;
    }
    return LogicalRect{.x = x, .y = y, .width = width, .height = height};
  }

  [[nodiscard]] std::string defaultFilenamePattern() { return "screenshot_%Y%m%d_%H%M%S"; }
  [[nodiscard]] std::string primaryKeybindLabel(const std::vector<KeyChord>& configured, KeybindAction action) {
    if (!configured.empty()) {
      return keyChordDisplayLabel(configured.front());
    }
    const auto defaults = defaultKeybindSet(action);
    return defaults.empty() ? std::string{} : keyChordDisplayLabel(defaults.front());
  }

  [[nodiscard]] std::string formatFilenameStem(std::string_view pattern, const std::string& labelBase, int suffix) {
    const auto now = std::chrono::system_clock::now();
    const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::string resolvedPattern = pattern.empty() ? defaultFilenamePattern() : std::string(pattern);
    std::string stem = formatLocalUnixTime(unixSeconds, resolvedPattern);
    if (stem.empty()) {
      stem = "screenshot";
    }

    if (suffix > 0) {
      stem += '-';
      stem += std::to_string(suffix);
    }
    if (labelBase != "screenshot") {
      stem += '-';
      stem += labelBase;
    }
    return stem;
  }

  [[nodiscard]] bool hasAnyOutput(const ScreenshotService::OutputOptions& options) {
    return options.extractText
        || options.saveToFile
        || options.copyToClipboard
        || (options.pipeToCommand && !options.pipeCommand.empty());
  }

  [[nodiscard]] bool needsScreenshotPath(const ScreenshotService::OutputOptions& options) {
    return options.saveToFile || (options.pipeToCommand && !options.pipeCommand.empty());
  }

  // Decoded file pixels stand in for a capture: straight RGBA, no cursor variant.
  [[nodiscard]] std::expected<capture::ScreenshotImage, std::string> loadImageForAnnotation(const std::string& path) {
    auto loaded = loadImageFile(path);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    if (loaded->width <= 0 || loaded->height <= 0) {
      return std::unexpected("image has no pixels");
    }
    return capture::ScreenshotImage{
        .image = ScreencopyImage{.width = loaded->width, .height = loaded->height, .rgba = std::move(loaded->rgba)},
        .cursorStatus = capture::CursorToggleStatus::NotCaptured,
    };
  }

  [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool resampleRgbaImage(ScreencopyImage& image, int targetWidth, int targetHeight) {
    if (targetWidth <= 0 || targetHeight <= 0 || image.width <= 0 || image.height <= 0) {
      return false;
    }
    if (image.width == targetWidth && image.height == targetHeight) {
      return true;
    }

    std::vector<std::uint8_t> resized(
        static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight) * 4U
    );
    if (stbir_resize_uint8_srgb(
            image.rgba.data(), image.width, image.height, 0, resized.data(), targetWidth, targetHeight, 0, STBIR_RGBA
        )
        == nullptr) {
      return false;
    }

    image.width = targetWidth;
    image.height = targetHeight;
    image.rgba = std::move(resized);
    return true;
  }

  [[nodiscard]] bool resampleScreenshotImage(capture::ScreenshotImage& image, int width, int height) {
    return resampleRgbaImage(image.image, width, height)
        && (!image.alternative || resampleRgbaImage(*image.alternative, width, height));
  }

  [[nodiscard]] capture::FrozenScreenshot*
  findFrozenScreenshot(std::vector<capture::FrozenScreenshot>& screenshots, wl_output* output) {
    for (auto& entry : screenshots) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  void attachStdioToDevNull() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
  }

  bool writeAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t written = ::write(fd, data + offset, size - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (written == 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  void pipePngToCommandAsync(
      std::string command, std::vector<std::uint8_t> png, const std::optional<std::filesystem::path>& screenshotPath
  ) {
    if (command.empty() || png.empty()) {
      return;
    }
    std::string screenshotPathString = screenshotPath.has_value() ? screenshotPath->string() : std::string{};

    std::thread([command = std::move(command), png = std::move(png),
                 screenshotPathString = std::move(screenshotPathString)]() {
      // Block SIGPIPE on this thread so a command that stops reading stdin makes
      // write() fail with EPIPE instead of terminating the whole process.
      sigset_t pipeMask;
      sigemptyset(&pipeMask);
      sigaddset(&pipeMask, SIGPIPE);
      pthread_sigmask(SIG_BLOCK, &pipeMask, nullptr);

      int stdinPipe[2] = {-1, -1};
      if (::pipe(stdinPipe) != 0) {
        kLog.warn("screenshot pipe: failed to create stdin pipe");
        return;
      }

      const pid_t child = ::fork();
      if (child < 0) {
        kLog.warn("screenshot pipe: fork failed");
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        return;
      }

      if (child == 0) {
        ::close(stdinPipe[1]);
        if (::dup2(stdinPipe[0], STDIN_FILENO) < 0) {
          ::_exit(126);
        }
        ::close(stdinPipe[0]);
        attachStdioToDevNull();
        if (screenshotPathString.empty()) {
          ::unsetenv(kScreenshotPathEnv);
        } else if (::setenv(kScreenshotPathEnv, screenshotPathString.c_str(), 1) != 0) {
          ::_exit(126);
        }
        // Restore default SIGPIPE handling for the spawned command.
        ::signal(SIGPIPE, SIG_DFL);
        pthread_sigmask(SIG_UNBLOCK, &pipeMask, nullptr);
        const char* argv[] = {"/bin/sh", "-lc", command.c_str(), nullptr};
        ::execv("/bin/sh", const_cast<char* const*>(argv));
        ::_exit(127);
      }

      ::close(stdinPipe[0]);
      const bool wrote = writeAll(stdinPipe[1], png.data(), png.size());
      ::close(stdinPipe[1]);
      if (!wrote) {
        kLog.warn("screenshot pipe: failed to write PNG to command stdin");
      }

      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        kLog.warn("screenshot pipe: command exited with status {}", status);
      }
    }).detach();
  }

  [[nodiscard]] std::vector<wl_output*> validOutputs(const WaylandConnection& wayland) {
    std::vector<wl_output*> outputs;
    for (const auto& output : wayland.outputs()) {
      if (output.output != nullptr && output.logicalWidth > 0 && output.logicalHeight > 0) {
        outputs.push_back(output.output);
      }
    }
    return outputs;
  }

  [[nodiscard]] std::expected<wl_output*, std::string>
  resolveOutputSelector(const WaylandConnection& wayland, std::string_view selector) {
    const std::string token = StringUtils::trim(selector);
    if (token.empty()) {
      return std::unexpected("error: empty monitor selector\n");
    }

    std::vector<wl_output*> matches;
    std::vector<std::string> knownOutputs;
    for (const auto& output : wayland.outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }
      if (!output.connectorName.empty()) {
        knownOutputs.push_back(output.connectorName);
      }
      if (outputMatchesSelector(token, output)) {
        matches.push_back(output.output);
      }
    }

    std::ranges::sort(knownOutputs);
    knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
    std::ranges::sort(matches, [](wl_output* a, wl_output* b) {
      return reinterpret_cast<std::uintptr_t>(a) < reinterpret_cast<std::uintptr_t>(b);
    });
    matches.erase(std::ranges::unique(matches).begin(), matches.end());

    if (matches.empty()) {
      std::string error = "error: unknown monitor selector \"" + token + "\"";
      if (!knownOutputs.empty()) {
        error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
      }
      error += "\n";
      return std::unexpected(std::move(error));
    }
    if (matches.size() > 1) {
      std::vector<std::string> matchNames;
      matchNames.reserve(matches.size());
      for (wl_output* output : matches) {
        if (const auto* entry = findOutput(wayland, output); entry != nullptr && !entry->connectorName.empty()) {
          matchNames.push_back(entry->connectorName);
        }
      }
      return std::unexpected(
          "error: monitor selector \""
          + token
          + "\" matched multiple outputs: "
          + StringUtils::join(matchNames, ", ")
          + "\n"
      );
    }

    return matches.front();
  }

  struct CapturedOutputFrame {
    capture::ScreenshotImage image;
    const WaylandOutput* output = nullptr;
  };

  struct RegionIntersectTarget {
    wl_output* output = nullptr;
    LogicalRect localRegion{};
  };

  struct GlobalRegionPiece {
    const WaylandOutput* output = nullptr;
    LogicalRect localRegion{};
    capture::ScreenshotImage image;
  };

  void blitOpaqueRgba(ScreencopyImage& canvas, int destX, int destY, const ScreencopyImage& source) {
    if (destX < 0 || destY < 0 || source.width <= 0 || source.height <= 0) {
      return;
    }
    const int copyWidth = std::min(source.width, canvas.width - destX);
    const int copyHeight = std::min(source.height, canvas.height - destY);
    if (copyWidth <= 0 || copyHeight <= 0) {
      return;
    }

    for (int y = 0; y < copyHeight; ++y) {
      const auto* srcRow =
          source.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) * 4U;
      auto* dstRow = canvas.rgba.data()
          + (static_cast<std::size_t>(destY + y) * static_cast<std::size_t>(canvas.width)
             + static_cast<std::size_t>(destX))
              * 4U;
      std::memcpy(dstRow, srcRow, static_cast<std::size_t>(copyWidth) * 4U);
    }
  }

  template <typename Piece>
  [[nodiscard]] capture::ScreenshotImage makeScreenshotCanvas(int width, int height, std::vector<Piece>& pieces) {
    capture::ScreenshotImage canvas{
        .image = {.width = width, .height = height},
        .cursorVisible = pieces.front().image.cursorVisible,
        .cursorStatus = capture::CursorToggleStatus::Available,
    };
    for (const auto& piece : pieces) {
      if (!piece.image.canToggleCursor()) {
        canvas.cursorStatus = piece.image.cursorStatus;
        break;
      }
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    canvas.image.rgba.assign(bytes, 0);
    const bool hasCursor =
        std::ranges::any_of(pieces, [](const auto& piece) { return piece.image.alternative.has_value(); });
    if (canvas.canToggleCursor() && hasCursor) {
      canvas.alternative = ScreencopyImage{.width = width, .height = height};
      canvas.alternative->rgba.assign(bytes, 0);
    } else {
      for (auto& piece : pieces) {
        if (piece.image.alternative && piece.image.cursorVisible != canvas.cursorVisible) {
          std::swap(piece.image.image, *piece.image.alternative);
          piece.image.cursorVisible = canvas.cursorVisible;
        }
        piece.image.alternative.reset();
      }
    }
    return canvas;
  }

  void blitScreenshot(capture::ScreenshotImage& canvas, int x, int y, const capture::ScreenshotImage& source) {
    blitOpaqueRgba(canvas.image, x, y, source.imageForCursor(canvas.cursorVisible));
    if (canvas.alternative) {
      blitOpaqueRgba(*canvas.alternative, x, y, source.imageForCursor(!canvas.cursorVisible));
    }
  }

  [[nodiscard]] std::vector<RegionIntersectTarget>
  intersectGlobalRegion(const WaylandConnection& wayland, LogicalRect globalRegion) {
    const int globalX0 = globalRegion.x;
    const int globalY0 = globalRegion.y;
    const int globalX1 = globalRegion.x + globalRegion.width;
    const int globalY1 = globalRegion.y + globalRegion.height;

    std::vector<RegionIntersectTarget> targets;
    for (const auto& out : wayland.outputs()) {
      if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
        continue;
      }
      const int ix0 = std::max(globalX0, out.logicalX);
      const int iy0 = std::max(globalY0, out.logicalY);
      const int ix1 = std::min(globalX1, out.logicalX + out.logicalWidth);
      const int iy1 = std::min(globalY1, out.logicalY + out.logicalHeight);
      if (ix1 <= ix0 || iy1 <= iy0) {
        continue;
      }
      targets.push_back(
          RegionIntersectTarget{
              .output = out.output,
              .localRegion = {
                  .x = ix0 - out.logicalX,
                  .y = iy0 - out.logicalY,
                  .width = ix1 - ix0,
                  .height = iy1 - iy0,
              },
          }
      );
    }
    return targets;
  }

  [[nodiscard]] int scaleLogicalFloor(int logical, double scale) {
    return static_cast<int>(std::floor(static_cast<double>(logical) * scale));
  }

  [[nodiscard]] int scaleLogicalCeil(int logical, double scale) {
    return static_cast<int>(std::ceil(static_cast<double>(logical) * scale));
  }

  [[nodiscard]] std::optional<capture::ScreenshotImage>
  composeGlobalRegion(LogicalRect globalRegion, std::vector<GlobalRegionPiece> pieces) {
    if (globalRegion.width <= 0 || globalRegion.height <= 0 || pieces.empty()) {
      return std::nullopt;
    }

    if (pieces.size() == 1) {
      return std::move(pieces.front().image);
    }

    double canvasScale = 1.0;
    for (const auto& piece : pieces) {
      if (piece.output == nullptr || piece.localRegion.width <= 0 || piece.localRegion.height <= 0) {
        return std::nullopt;
      }
      canvasScale = std::max({
          canvasScale,
          static_cast<double>(piece.image.image.width) / static_cast<double>(piece.localRegion.width),
          static_cast<double>(piece.image.image.height) / static_cast<double>(piece.localRegion.height),
      });
    }

    const int canvasWidth = scaleLogicalCeil(globalRegion.width, canvasScale);
    const int canvasHeight = scaleLogicalCeil(globalRegion.height, canvasScale);
    if (canvasWidth <= 0 || canvasHeight <= 0) {
      return std::nullopt;
    }

    auto canvas = makeScreenshotCanvas(canvasWidth, canvasHeight, pieces);

    for (auto& piece : pieces) {
      const int globalPieceX = piece.output->logicalX + piece.localRegion.x;
      const int globalPieceY = piece.output->logicalY + piece.localRegion.y;
      const int offsetX = globalPieceX - globalRegion.x;
      const int offsetY = globalPieceY - globalRegion.y;
      const int destX = scaleLogicalFloor(offsetX, canvasScale);
      const int destY = scaleLogicalFloor(offsetY, canvasScale);
      const int targetWidth = scaleLogicalCeil(offsetX + piece.localRegion.width, canvasScale) - destX;
      const int targetHeight = scaleLogicalCeil(offsetY + piece.localRegion.height, canvasScale) - destY;
      if (!resampleScreenshotImage(piece.image, targetWidth, targetHeight)) {
        return std::nullopt;
      }
      blitScreenshot(canvas, destX, destY, piece.image);
    }

    return canvas;
  }

  [[nodiscard]] std::optional<capture::ScreenshotImage> stitchOutputFrames(std::vector<CapturedOutputFrame> frames) {
    if (frames.empty()) {
      return std::nullopt;
    }

    for (const auto& frame : frames) {
      if (frame.output == nullptr || frame.output->logicalWidth <= 0 || frame.output->logicalHeight <= 0) {
        return std::nullopt;
      }
    }

    if (frames.size() == 1) {
      return std::move(frames.front().image);
    }

    // Stitch in physical pixels: pick a uniform canvas density equal to the highest captured
    // scale so the sharpest monitor keeps its full resolution. Each logical layout coordinate is
    // multiplied by this density; lower-density outputs are upscaled to keep the layout aligned.
    double canvasScale = 1.0;
    for (const auto& frame : frames) {
      const double scaleX =
          static_cast<double>(frame.image.image.width) / static_cast<double>(frame.output->logicalWidth);
      const double scaleY =
          static_cast<double>(frame.image.image.height) / static_cast<double>(frame.output->logicalHeight);
      canvasScale = std::max({canvasScale, scaleX, scaleY});
    }

    int minLogicalX = frames.front().output->logicalX;
    int minLogicalY = frames.front().output->logicalY;
    for (const auto& frame : frames) {
      minLogicalX = std::min(minLogicalX, frame.output->logicalX);
      minLogicalY = std::min(minLogicalY, frame.output->logicalY);
    }

    const auto outputPixelRect = [canvasScale, minLogicalX, minLogicalY](const WaylandOutput& output) {
      const int offsetX = output.logicalX - minLogicalX;
      const int offsetY = output.logicalY - minLogicalY;
      const int x = scaleLogicalFloor(offsetX, canvasScale);
      const int y = scaleLogicalFloor(offsetY, canvasScale);
      return LogicalRect{
          .x = x,
          .y = y,
          .width = scaleLogicalCeil(offsetX + output.logicalWidth, canvasScale) - x,
          .height = scaleLogicalCeil(offsetY + output.logicalHeight, canvasScale) - y,
      };
    };

    int canvasWidth = 0;
    int canvasHeight = 0;
    for (auto& frame : frames) {
      const LogicalRect pixelRect = outputPixelRect(*frame.output);
      canvasWidth = std::max(canvasWidth, pixelRect.x + pixelRect.width);
      canvasHeight = std::max(canvasHeight, pixelRect.y + pixelRect.height);
    }

    if (canvasWidth <= 0 || canvasHeight <= 0) {
      return std::nullopt;
    }

    auto canvas = makeScreenshotCanvas(canvasWidth, canvasHeight, frames);

    for (auto& frame : frames) {
      const LogicalRect pixelRect = outputPixelRect(*frame.output);
      if (!resampleScreenshotImage(frame.image, pixelRect.width, pixelRect.height)) {
        return std::nullopt;
      }
      blitScreenshot(canvas, pixelRect.x, pixelRect.y, frame.image);
    }

    return canvas;
  }

} // namespace

ScreenshotService::ScreenshotService(
    WaylandConnection& wayland, CompositorPlatform& platform, ConfigService& configService,
    NotificationManager& notifications, ClipboardService* clipboard
)
    : m_wayland(wayland), m_platform(platform), m_notifications(notifications), m_configService(configService),
      m_clipboard(clipboard), m_capture(wayland), m_windowCapture(wayland) {
  ScreenRecorder::instance().activeChanged = [this](bool active) { m_notifications.setRecordingActive(active); };
  m_notifications.addInternalActionCallback(
      [this](std::uint32_t id, const std::string& actionKey, const std::string& activationToken) {
        onSavedNotificationAction(id, actionKey, activationToken);
      }
  );
}

ScreenshotService::~ScreenshotService() {
  m_menuTimer.stop();
  m_windowRefresh.stop();
  m_windowCapture.cancelInFlight();
  m_windowLifetime.reset();
  if (m_windowCancel)
    *m_windowCancel = true;
  m_recordingPreviewLifetime.reset();
  if (m_recordingPreviewCancel)
    *m_recordingPreviewCancel = true;
  m_textLifetime.reset();
  if (m_textCancel)
    *m_textCancel = true;
  // Application UI members have already been destroyed by this point.
  ScreenRecorder::instance().activeChanged = {};
  ScreenRecorder::instance().shutdown();
}

void ScreenshotService::rememberRegion(const LogicalRect& region) {
  if (region.width < 2 || region.height < 2) {
    return;
  }
  (void)m_configService.setStateString(kStateOwner, kLastRegionKey, encodeRegion(region));
}

std::optional<LogicalRect> ScreenshotService::loadRememberedRegion() const {
  const auto text = m_configService.stateString(kStateOwner, kLastRegionKey);
  if (!text.has_value()) {
    return std::nullopt;
  }
  auto region = parseRegion(*text);
  if (!region.has_value()) {
    return std::nullopt;
  }
  if (intersectGlobalRegion(m_wayland, *region).empty()) {
    return std::nullopt;
  }
  return region;
}

bool ScreenshotService::available() const noexcept { return m_capture.available(); }

void ScreenshotService::onOutputChange() {
  cancelMenuCapture();
  if (m_regionOverlay != nullptr) {
    m_regionOverlay->onOutputChange();
  }
  if (m_annotationOverlay != nullptr) {
    m_annotationOverlay->onOutputChange();
  }
}

bool ScreenshotService::onPointerEvent(const PointerEvent& event) {
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    return m_annotationOverlay->onPointerEvent(event);
  }
  if (m_regionOverlay == nullptr || !m_regionOverlay->isActive()) {
    return false;
  }
  return m_regionOverlay->onPointerEvent(event);
}

bool ScreenshotService::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_menuCapture && event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    cancelMenuCapture();
    return true;
  }
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    if (m_annotationOverlay->onKeyboardEvent(event)) {
      return true;
    }
  }
  if (!event.pressed) {
    return false;
  }
  const bool regionActive = m_regionOverlay != nullptr && m_regionOverlay->isActive();
  if (!m_freezeCaptureActive && !regionActive) {
    return false;
  }
  if (regionActive && m_regionOverlay->onKeyboardEvent(event)) {
    return true;
  }
  if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    return false;
  }
  cancelRegionCapture();
  return true;
}

bool ScreenshotService::overlayBusy() const noexcept {
  return m_menuCapture.has_value()
      || (m_regionOverlay != nullptr && m_regionOverlay->isActive())
      || (m_annotationOverlay != nullptr && m_annotationOverlay->isActive())
      || m_freezeCaptureActive;
}

ScreenshotService::OutputOptions ScreenshotService::outputOptionsFromConfig(const Config& config) {
  const auto& screenshot = config.shell.screenshot;
  OutputOptions options;
  options.saveToFile = screenshot.saveToFile;
  options.copyToClipboard = screenshot.copyToClipboard;
  options.pipeToCommand = screenshot.pipeToCommand;
  options.freezeScreen = screenshot.freezeScreen;
  options.confirmRegion = screenshot.confirmRegion;
  options.rememberLastRegion = screenshot.rememberLastRegion;
  options.showCursor = screenshot.showCursor;
  options.annotate = screenshot.annotate;
  options.pipeCommand = screenshot.pipeCommand;
  options.directory = screenshot.directory;
  options.filenamePattern = screenshot.filenamePattern;
  return options;
}

std::string ScreenshotService::beginMenuCapture(capture::LaunchOptions options) {
  if (textCaptureAllowed && !textCaptureAllowed())
    return i18n::tr("island.capture-menu.locked");
  if (!available())
    return i18n::tr("island.capture-menu.unavailable");
  if (overlayBusy() || m_capture.busy() || m_textCancel)
    return i18n::tr("island.capture-menu.busy");
  if (options.recording && ScreenRecorder::instance().active())
    return i18n::tr("island.capture-menu.recording-active");
  if (options.recording && !process::commandExists("wf-recorder"))
    return i18n::tr("island.capture-menu.missing-recorder");
  if (options.target == capture::Target::Window
      && (options.recording || !m_platform.hyprlandRuntime().available() || !m_windowCapture.available()))
    return i18n::tr("island.capture-menu.window-unavailable");
  auto* context = PanelManager::instance().renderContext();
  if (!context)
    return i18n::tr("island.capture-menu.unavailable");
  options.delaySeconds = std::clamp(options.delaySeconds, 0, 10);
  m_regionRenderContext = context;
  m_regionOutputOptions = outputOptionsFromConfig(m_configService.config());
  if (!options.recording && !m_regionOutputOptions.annotate && !hasAnyOutput(m_regionOutputOptions))
    return i18n::tr("island.capture-menu.no-output");
  // Selection stays live, so a delayed screenshot contains the frame after the countdown.
  m_regionOutputOptions.freezeScreen = false;
  const bool monitor = options.target == capture::Target::Monitor;
  const bool window = options.target == capture::Target::Window;
  m_regionFullscreenPick = monitor;
  m_recordSelection = false;
  m_menuSelection = options;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false, monitor, false, std::nullopt, window);
  if (window)
    refreshWindowTargets();
  return {};
}

void ScreenshotService::requestWindowTargets(
    std::function<void(std::optional<std::vector<capture::WindowTarget>>)> callback
) {
  if (m_windowCancel)
    *m_windowCancel = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_windowCancel = cancel;
  const std::weak_ptr<int> lifetime = m_windowLifetime;
  const bool launched = process::runAsync(
      std::vector<std::string>{"hyprctl", "-j", "clients"},
      {.onExit =
           [lifetime, cancel, callback](process::RunResult result) {
             auto windows =
                 result && !result.outTruncated ? capture::parseHyprlandCaptureWindows(result.out) : std::nullopt;
             DeferredCall::callLater([lifetime, cancel, callback, windows = std::move(windows)]() mutable {
               if (!lifetime.expired() && !*cancel)
                 callback(std::move(windows));
             });
           }},
      {.timeout = std::chrono::seconds(1), .maxOutputBytes = 2 * 1024 * 1024, .cancel = cancel}
  );
  if (!launched)
    callback(std::nullopt);
}

void ScreenshotService::refreshWindowTargets() {
  if (!m_menuSelection || m_menuSelection->target != capture::Target::Window || !m_regionOverlay->isActive())
    return;
  requestWindowTargets([this](std::optional<std::vector<capture::WindowTarget>> windows) {
    if (!windows) {
      cancelMenuCapture();
      notifyError(i18n::tr("island.capture-menu.window-unavailable"));
      return;
    }
    m_regionOverlay->setWindowTargets(std::move(*windows));
    m_windowRefresh.start(std::chrono::milliseconds(350), [this] { refreshWindowTargets(); });
  });
}

void ScreenshotService::cancelMenuCapture() {
  const bool selecting = m_menuSelection.has_value();
  const bool pending = m_menuCapture.has_value();
  m_menuTimer.stop();
  m_windowRefresh.stop();
  m_windowCapture.cancelInFlight();
  if (m_windowCancel)
    *m_windowCancel = true;
  m_menuSelection.reset();
  m_menuCapture.reset();
  if (selecting)
    cancelRegionCapture();
  if (pending && captureCountdown)
    captureCountdown(false, 0, {});
}

void ScreenshotService::finishMenuSelection(LogicalRect region, wl_output* output, capture::LaunchOptions options) {
  const bool monitor = options.target == capture::Target::Monitor;
  const WaylandOutput* selected = monitor ? findOutput(m_wayland, output) : nullptr;
  if (!monitor)
    for (const auto& candidate : m_wayland.outputs())
      if (region.x >= candidate.logicalX
          && region.y >= candidate.logicalY
          && region.x + region.width <= candidate.logicalX + candidate.logicalWidth
          && region.y + region.height <= candidate.logicalY + candidate.logicalHeight) {
        selected = &candidate;
        break;
      }
  if ((options.recording || monitor) && !selected) {
    notifyError(i18n::tr("island.capture-menu.one-monitor"));
    return;
  }
  m_menuCapture = MenuCapture{
      .launch = options,
      .outputOptions = m_regionOutputOptions,
      .region = region,
      .output = selected ? selected->connectorName : std::string{},
      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.delaySeconds)
  };
  m_menuTimer.startRepeating(std::chrono::milliseconds(100), [this] { tickMenuCapture(); });
  tickMenuCapture();
}

void ScreenshotService::tickMenuCapture() {
  if (!m_menuCapture)
    return;
  if (textCaptureAllowed && !textCaptureAllowed()) {
    cancelMenuCapture();
    return;
  }
  const int remaining = static_cast<int>(
      std::chrono::ceil<std::chrono::seconds>(m_menuCapture->deadline - std::chrono::steady_clock::now()).count()
  );
  if (remaining <= 0) {
    if (captureCountdown)
      captureCountdown(false, 0, {});
    // Let the selector and countdown disappear from the compositor before capturing.
    m_menuTimer.start(std::chrono::milliseconds(200), [this] { runMenuCapture(); });
  } else if (remaining != m_menuCapture->lastRemaining) {
    m_menuCapture->lastRemaining = remaining;
    if (captureCountdown)
      captureCountdown(m_menuCapture->launch.recording, remaining, m_menuCapture->output);
  }
}

void ScreenshotService::runMenuCapture() {
  auto pending = std::exchange(m_menuCapture, std::nullopt);
  if (!pending || (textCaptureAllowed && !textCaptureAllowed()))
    return;
  const auto& request = *pending;
  if (request.launch.target == capture::Target::Window) {
    m_menuCapture = request;
    // Resolve the same window again after the countdown, following moves and resizes.
    requestWindowTargets([this, request](std::optional<std::vector<capture::WindowTarget>> windows) {
      if (textCaptureAllowed && !textCaptureAllowed()) {
        m_menuCapture.reset();
        return;
      }
      if (windows) {
        const auto found = std::ranges::find(*windows, request.windowId, &capture::WindowTarget::id);
        if (found != windows->end()) {
          ext_foreign_toplevel_handle_v1* selected = nullptr;
          m_wayland.visitExtToplevelHandles([&](auto* handle) {
            if (m_platform.compositorWindowIdForExtToplevel(handle) == request.windowId)
              selected = handle;
          });
          if (selected) {
            const auto destPath = needsScreenshotPath(request.outputOptions)
                ? std::optional(makeScreenshotPath(request.outputOptions, "window"))
                : std::nullopt;
            // Keep native pixels and exclude desktop overlays, other windows and the cursor.
            m_windowCapture.capture(
                selected, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                [this, options = request.outputOptions,
                 destPath](std::optional<ScreencopyImage> image, std::string error) {
                  m_menuCapture.reset();
                  if (textCaptureAllowed && !textCaptureAllowed())
                    return;
                  std::optional<capture::ScreenshotImage> result;
                  if (image) {
                    playCaptureSound();
                    result = capture::ScreenshotImage{.image = std::move(*image)};
                  }
                  onCaptureComplete(std::move(result), error, options, destPath);
                }
            );
            return;
          }
        }
      }
      m_menuCapture.reset();
      notifyError(i18n::tr("island.capture-menu.window-gone"));
    });
    return;
  }
  const auto found = std::ranges::find(m_wayland.outputs(), request.output, &WaylandOutput::connectorName);
  if (!request.output.empty() && found == m_wayland.outputs().end()) {
    notifyError(i18n::tr("island.capture-menu.unavailable"));
    return;
  }
  if (request.launch.recording) {
    const auto& region = request.region;
    const auto geometry = request.launch.target == capture::Target::Monitor
        ? std::string{}
        : std::format("{},{} {}x{}", region.x, region.y, region.width, region.height);
    const auto error = ScreenRecorder::instance().start(request.output, geometry, request.launch.audio);
    if (!error.empty())
      notifyError(error);
  } else {
    if (request.launch.target == capture::Target::Region && request.outputOptions.rememberLastRegion)
      rememberRegion(request.region);
    playCaptureSound();
    if (request.launch.target == capture::Target::Monitor)
      completeFullscreenSelection(found->output, request.outputOptions);
    else
      captureGlobalRegion(request.region, request.outputOptions);
  }
}

std::string ScreenshotService::beginRecording(bool monitor) {
  if (ScreenRecorder::instance().active())
    return "error: recording already active\n";
  if (overlayBusy())
    return "error: a capture overlay is already active\n";
  auto* context = PanelManager::instance().renderContext();
  if (!context || !available())
    return "error: capture unavailable\n";
  m_regionRenderContext = context;
  m_regionOutputOptions = {};
  m_regionFullscreenPick = monitor;
  m_recordSelection = true;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false, monitor, false);
  return "ok\n";
}

void ScreenshotService::registerIpc(IpcService& ipc, const ConfigService& configService) {
  ipc.bind(noctalia::cli::msg::textCapture, [this](const std::string&) -> std::string {
    if (textCaptureAllowed && !textCaptureAllowed())
      return "error: session is locked\n";
    if (!available())
      return "error: screen capture unavailable\n";
    if (overlayBusy() || m_textCancel)
      return "error: a capture is already in progress\n";
    if (!process::commandExists("tesseract")) {
      if (textCaptureFeedback)
        textCaptureFeedback(i18n::tr("utilities.text.missing-engine"));
      return "error: install tesseract and its language data to extract text\n";
    }
    if (!m_clipboard || !m_clipboard->isAvailable())
      return "error: clipboard unavailable\n";
    auto* context = PanelManager::instance().renderContext();
    if (!context)
      return "error: render context unavailable\n";
    beginRegionCapture(*context, OutputOptions{.saveToFile = false, .extractText = true});
    return "ok\n";
  });
  ScreenRecorder::instance().completed = [this](const ScreenRecorder::Result& result) {
    if (result.success)
      notifyRecordingSaved(result);
    else
      m_notifications.addInternal(
          "Noctalia", i18n::tr("notifications.internal.recording-failed"), result.error, Urgency::Critical
      );
  };
  ipc.bind(noctalia::cli::msg::recordRegion, [this](const std::string&) { return beginRecording(false); });
  ipc.bind(noctalia::cli::msg::recordMonitor, [this](const std::string&) { return beginRecording(true); });
  ipc.bind(noctalia::cli::msg::recordStop, [this](const std::string&) {
    if ((m_menuSelection && m_menuSelection->recording) || (m_menuCapture && m_menuCapture->launch.recording))
      cancelMenuCapture();
    ScreenRecorder::instance().stop();
    return std::string("ok\n");
  });
  ipc.bind(noctalia::cli::msg::recordStatus, [](const std::string&) {
    auto& recorder = ScreenRecorder::instance();
    return recorder.active() ? recorder.label() + "\n" : std::string("idle\n");
  });

  ipc.bind(noctalia::cli::msg::screenshotRegion, [this, &configService](const std::string& /*args*/) -> std::string {
    if (!available()) {
      return "error: screen capture is not available on this compositor\n";
    }
    if (overlayBusy()) {
      return "error: a screenshot overlay is already active\n";
    }
    auto* renderContext = PanelManager::instance().renderContext();
    if (renderContext == nullptr) {
      return "error: render context unavailable\n";
    }
    beginRegionCapture(*renderContext, outputOptionsFromConfig(configService.config()));
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::screenshotFullscreen, [this, &configService](const std::string& args) -> std::string {
    if (!available()) {
      return "error: screen capture is not available on this compositor\n";
    }
    if (overlayBusy()) {
      return "error: a screenshot overlay is already active\n";
    }
    const std::string token = StringUtils::trim(args);
    const auto options = outputOptionsFromConfig(configService.config());
    if (token == "all" || token == "*") {
      captureAllOutputs(options);
      return "ok\n";
    }
    if (token == "pick") {
      const auto outputs = validOutputs(m_wayland);
      if (outputs.size() <= 1) {
        captureFullscreen(options, outputs.empty() ? nullptr : outputs.front());
        return "ok\n";
      }
      auto* renderContext = PanelManager::instance().renderContext();
      if (renderContext == nullptr) {
        return "error: render context unavailable\n";
      }
      beginFullscreenCapture(*renderContext, options);
      return "ok\n";
    }
    if (!token.empty() && token != "pick") {
      auto output = resolveOutputSelector(m_wayland, token);
      if (!output) {
        return output.error();
      }
      captureFullscreen(options, *output);
      return "ok\n";
    }

    captureFullscreen(options);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::screenshotAnnotate, [this, &configService](const std::string& /*args*/) -> std::string {
    if (!available()) {
      return "error: screen capture is not available on this compositor\n";
    }
    if (overlayBusy()) {
      return "error: a screenshot overlay is already active\n";
    }
    auto* renderContext = PanelManager::instance().renderContext();
    if (renderContext == nullptr) {
      return "error: render context unavailable\n";
    }
    beginAnnotation(*renderContext, outputOptionsFromConfig(configService.config()), true);
    return "ok\n";
  });

  // The live annotator draws over running apps, so it opens without screencopy;
  // only its Freeze action needs capture support. With a path it edits that image instead.
  ipc.bind(noctalia::cli::msg::annotate, [this, &ipc, &configService](const std::string& args) -> std::string {
    if (overlayBusy()) {
      return "error: a screenshot overlay is already active\n";
    }
    auto* renderContext = PanelManager::instance().renderContext();
    if (renderContext == nullptr) {
      return "error: render context unavailable\n";
    }
    const auto options = outputOptionsFromConfig(configService.config());
    const std::string path = StringUtils::trim(args);
    if (path.empty()) {
      beginAnnotation(*renderContext, options, false);
      return "ok\n";
    }
    const std::optional<std::string_view> callerCwd =
        ipc.callerCwd().has_value() ? std::optional<std::string_view>{*ipc.callerCwd()} : std::nullopt;
    const std::string resolved = FileUtils::resolvePath(path, callerCwd).string();
    if (const auto started = beginImageFileAnnotation(*renderContext, resolved, options); !started) {
      return "error: " + started.error() + " (" + resolved + ")\n";
    }
    return "ok\n";
  });
}

wl_output* ScreenshotService::preferredCaptureOutput() const {
  if (wl_output* output = m_platform.preferredInteractiveOutput(); output != nullptr) {
    return output;
  }
  const auto outputs = validOutputs(m_wayland);
  return outputs.empty() ? nullptr : outputs.front();
}

void ScreenshotService::captureFullscreen(const OutputOptions& options, wl_output* output) {
  cancelMenuCapture();
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    notifyError("An annotation overlay is already active");
    return;
  }
  if (output == nullptr) {
    output = preferredCaptureOutput();
  }
  if (output == nullptr) {
    notifyError("No outputs available");
    return;
  }
  playCaptureSound();
  captureOutput(output, std::nullopt, "screenshot", options);
}

void ScreenshotService::captureFullscreenInteractive(RenderContext& renderContext, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (validOutputs(m_wayland).size() <= 1) {
    captureFullscreen(options);
    return;
  }
  beginFullscreenCapture(renderContext, options);
}

void ScreenshotService::beginRegionCapture(RenderContext& renderContext, const OutputOptions& options) {
  cancelMenuCapture();
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!options.annotate && !hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    notifyError("An annotation overlay is already active");
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancel();
  }
  if (m_freezeCaptureActive) {
    abortFreezeCapture("Screenshot cancelled");
  }

  m_regionOutputOptions = options;
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = false;

  if (options.freezeScreen) {
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  startRegionOverlay(renderContext);
}

void ScreenshotService::beginFullscreenCapture(RenderContext& renderContext, const OutputOptions& options) {
  cancelMenuCapture();
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    notifyError("An annotation overlay is already active");
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancel();
  }
  if (m_freezeCaptureActive) {
    abortFreezeCapture("Screenshot cancelled");
  }

  m_regionOutputOptions = options;
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = true;

  if (options.freezeScreen) {
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  startFullscreenOverlay(renderContext);
}

void ScreenshotService::ensureRegionOverlay() {
  if (m_regionRenderContext == nullptr) {
    return;
  }
  if (m_regionOverlay == nullptr) {
    m_regionOverlay = std::make_unique<capture::ScreenshotRegionOverlay>();
  }
  m_regionOverlay->initialize(m_wayland, m_regionRenderContext);
  const auto& keybinds = m_configService.config().keybinds;
  m_regionOverlay->setConfirmKeybindLabels(
      primaryKeybindLabel(keybinds.copy, KeybindAction::Copy), primaryKeybindLabel(keybinds.save, KeybindAction::Save),
      primaryKeybindLabel(keybinds.cancel, KeybindAction::Cancel)
  );
  m_regionOverlay->setFailureCallback([this](const std::string& message) {
    m_windowRefresh.stop();
    if (m_windowCancel)
      *m_windowCancel = true;
    m_menuSelection.reset();
    m_frozenScreenshots.clear();
    m_regionFullscreenPick = false;
    m_recordSelection = false;
    notifyError(message);
  });

  m_regionOverlay->setWindowCompleteCallback([this](capture::WindowTarget window) {
    m_windowRefresh.stop();
    if (m_windowCancel)
      *m_windowCancel = true;
    const auto options = std::exchange(m_menuSelection, std::nullopt);
    if (!options || options->target != capture::Target::Window)
      return;
    m_platform.focusCompositorWindow(window.id);
    finishMenuSelection(window.bounds, nullptr, *options);
    if (m_menuCapture)
      m_menuCapture->windowId = window.id;
  });

  m_regionOverlay->setCompleteCallback(
      [this](std::optional<LogicalRect> region, wl_output* output, capture::ConfirmAction action) {
        if (auto menu = std::exchange(m_menuSelection, std::nullopt)) {
          m_windowRefresh.stop();
          if (m_windowCancel)
            *m_windowCancel = true;
          m_regionFullscreenPick = false;
          if (region)
            finishMenuSelection(*region, output, *menu);
          return;
        }
        if (m_recordSelection) {
          m_recordSelection = false;
          const bool monitor = std::exchange(m_regionFullscreenPick, false);
          if (!region)
            return;
          const WaylandOutput* selected = monitor ? findOutput(m_wayland, output) : nullptr;
          if (!monitor) {
            for (const auto& candidate : m_wayland.outputs()) {
              if (region->x >= candidate.logicalX
                  && region->y >= candidate.logicalY
                  && region->x + region->width <= candidate.logicalX + candidate.logicalWidth
                  && region->y + region->height <= candidate.logicalY + candidate.logicalHeight) {
                selected = &candidate;
                break;
              }
            }
          }
          if (!selected) {
            m_notifications.addInternal("Noctalia", "Recording not started", "Select an area within one monitor");
            return;
          }
          const auto geometry =
              monitor ? std::string{} : std::format("{},{} {}x{}", region->x, region->y, region->width, region->height);
          const auto error = ScreenRecorder::instance().start(selected->connectorName, geometry);
          if (!error.empty())
            m_notifications.addInternal("Noctalia", "Recording failed", error, Urgency::Critical);
          return;
        }

        if (!region.has_value()) {
          if (m_regionOverlay != nullptr) {
            if (auto abandoned = m_regionOverlay->takeAbandonedRegion();
                abandoned.has_value() && m_regionOutputOptions.rememberLastRegion) {
              rememberRegion(*abandoned);
            }
            m_regionOverlay->setFrozenScreenshots({});
          }
          m_frozenScreenshots.clear();
          m_regionFullscreenPick = false;
          return;
        }

        if (m_regionFullscreenPick) {
          if (output == nullptr) {
            m_frozenScreenshots.clear();
            if (m_regionOverlay != nullptr) {
              m_regionOverlay->setFrozenScreenshots({});
            }
            m_regionFullscreenPick = false;
            return;
          }
          if (m_regionOutputOptions.freezeScreen && m_regionOverlay != nullptr) {
            m_frozenScreenshots = m_regionOverlay->takeFrozenScreenshots();
          }
          playCaptureSound();
          completeFullscreenSelection(output, m_regionOutputOptions);
          m_regionFullscreenPick = false;
          return;
        }

        if (m_regionOutputOptions.rememberLastRegion) {
          rememberRegion(*region);
        }

        OutputOptions options = m_regionOutputOptions;
        if (action != capture::ConfirmAction::None
            && m_configService.config().shell.screenshot.skipAnnotateOnCopySave) {
          options.annotate = false;
        }
        if (action == capture::ConfirmAction::ForceClipboard) {
          options.copyToClipboard = true;
          options.saveToFile = false;
        } else if (action == capture::ConfirmAction::ForceSave) {
          options.copyToClipboard = false;
          options.saveToFile = true;
        }

        if (options.freezeScreen && m_regionOverlay != nullptr) {
          m_frozenScreenshots = m_regionOverlay->takeFrozenScreenshots();
        }
        playCaptureSound();
        if (options.freezeScreen && !m_frozenScreenshots.empty()) {
          deliverFrozenGlobalRegion(*region, options);
          return;
        }
        captureGlobalRegion(*region, options);
      }
  );
}

void ScreenshotService::startRegionOverlay(RenderContext& renderContext) {
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = false;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  const std::optional<LogicalRect> initial =
      m_regionOutputOptions.rememberLastRegion ? loadRememberedRegion() : std::nullopt;
  m_regionOverlay->begin(false, false, m_regionOutputOptions.confirmRegion, initial);
}

void ScreenshotService::startFullscreenOverlay(RenderContext& renderContext) {
  m_regionRenderContext = &renderContext;
  m_regionFullscreenPick = true;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false, true, false);
}

void ScreenshotService::beginFreezeCapture() {
  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    return;
  }

  m_frozenScreenshots.clear();
  m_pendingFreezeCaptures.clear();
  for (const auto& output : m_wayland.outputs()) {
    if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      continue;
    }
    m_pendingFreezeCaptures.push_back(FreezeRequest{.output = output.output});
  }
  if (m_pendingFreezeCaptures.empty()) {
    notifyError("No outputs available");
    return;
  }

  m_freezeCaptureActive = true;
  startNextFreezeCapture();
}

void ScreenshotService::startNextFreezeCapture() {
  if (!m_freezeCaptureActive) {
    return;
  }

  if (m_pendingFreezeCaptures.empty()) {
    m_freezeCaptureActive = false;
    finishFreezeCapture();
    return;
  }

  const FreezeRequest request = m_pendingFreezeCaptures.front();
  m_pendingFreezeCaptures.erase(m_pendingFreezeCaptures.begin());
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  m_capture.capture(
      request.output, std::nullopt, m_regionOutputOptions.showCursor,
      m_freezeTarget == FreezeTarget::Annotation || m_regionOutputOptions.annotate,
      [this, request](std::optional<capture::ScreenshotImage> image, const std::string& error) {
        onFreezeFrameCaptured(request, std::move(image), error);
      }
  );
}

void ScreenshotService::onFreezeFrameCaptured(
    FreezeRequest request, std::optional<capture::ScreenshotImage> image, const std::string& error
) {
  if (!m_freezeCaptureActive) {
    return;
  }

  if (!error.empty() || !image.has_value()) {
    kLog.warn("failed to freeze output: {}", error.empty() ? "empty frame" : error);
  } else {
    m_frozenScreenshots.push_back(capture::FrozenScreenshot{.output = request.output, .image = std::move(*image)});
  }

  DeferredCall::callLater([this]() { startNextFreezeCapture(); });
}

void ScreenshotService::finishFreezeCapture() {
  m_freezeCaptureActive = false;
  const FreezeTarget target = std::exchange(m_freezeTarget, FreezeTarget::Region);

  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    m_frozenScreenshots.clear();
    return;
  }

  if (target == FreezeTarget::Annotation) {
    if (m_frozenScreenshots.empty()) {
      notifyError("Failed to freeze the screen");
      if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
        m_annotationOverlay->resumeAfterCapture();
      }
      return;
    }
    ensureAnnotationOverlay();
    m_annotationOverlay->setFrozenScreenshots(std::move(m_frozenScreenshots));
    m_frozenScreenshots.clear();
    if (m_annotationOverlay->isActive()) {
      m_annotationOverlay->resumeAfterCapture();
    } else {
      m_annotationOverlay->begin();
    }
    return;
  }

  if (m_frozenScreenshots.empty()) {
    notifyError("Failed to freeze screen");
    return;
  }

  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots(std::move(m_frozenScreenshots));
  const std::optional<LogicalRect> initial =
      (!m_regionFullscreenPick && m_regionOutputOptions.rememberLastRegion) ? loadRememberedRegion() : std::nullopt;
  m_regionOverlay->begin(
      true, m_regionFullscreenPick, !m_regionFullscreenPick && m_regionOutputOptions.confirmRegion, initial
  );
}

void ScreenshotService::abortFreezeCapture(const std::string& message) {
  cancelAllOutputsBatch();
  m_freezeCaptureActive = false;
  m_pendingFreezeCaptures.clear();
  m_freezeTarget = FreezeTarget::Region;
  m_frozenScreenshots.clear();
  m_capture.cancelInFlight();
  if (!message.empty()) {
    notifyError(message);
  }
}

void ScreenshotService::cancelRegionCapture() {
  cancelAllOutputsBatch();
  if (m_freezeCaptureActive) {
    abortFreezeCapture({});
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancelSelection();
  }
}

capture::AnnotationToolState ScreenshotService::loadAnnotationToolState() const {
  capture::AnnotationToolState state = capture::defaultAnnotationToolState();
  if (const auto tool = m_configService.stateString(kAnnotateStateOwner, "tool"); tool.has_value()) {
    if (const auto parsed = capture::annotationToolFromName(*tool); parsed.has_value()) {
      state.tool = *parsed;
    }
  }
  if (const auto fill = m_configService.stateString(kAnnotateStateOwner, "fill"); fill.has_value()) {
    state.fill = *fill == "1";
  }
  if (const auto advanced = m_configService.stateString(kAnnotateStateOwner, "advanced_size"); advanced.has_value()) {
    state.advancedSize = *advanced == "1";
  }
  if (const auto encoded = m_configService.stateString(kAnnotateStateOwner, "toolbar_positions"); encoded.has_value()) {
    const nlohmann::json positions = nlohmann::json::parse(*encoded, nullptr, false);
    bool invalid = !positions.is_object();
    if (positions.is_object()) {
      for (const auto& [outputName, position] : positions.items()) {
        if (outputName.empty() || !position.is_object()) {
          invalid = true;
          continue;
        }
        const auto x = position.find("x");
        const auto y = position.find("y");
        if (x == position.end() || y == position.end() || !x->is_number() || !y->is_number()) {
          invalid = true;
          continue;
        }
        const double parsedX = x->get<double>();
        const double parsedY = y->get<double>();
        if (!std::isfinite(parsedX) || !std::isfinite(parsedY)) {
          invalid = true;
          continue;
        }
        state.toolbarPositions.insert_or_assign(outputName, capture::AnnotationPoint{.x = parsedX, .y = parsedY});
      }
    }
    if (invalid) {
      kLog.warn("invalid annotation toolbar position state");
    }
  }
  for (std::size_t i = 0; i < capture::kAnnotationToolCount; ++i) {
    const std::string name(capture::annotationToolName(static_cast<capture::AnnotationTool>(i)));
    if (const auto width = m_configService.stateString(kAnnotateStateOwner, std::format("width_{}", name));
        width.has_value()) {
      if (const auto parsed = parseDouble(*width); parsed.has_value()) {
        state.width[i] = *parsed;
      }
    }
    if (const auto color = m_configService.stateString(kAnnotateStateOwner, std::format("color_{}", name));
        color.has_value()) {
      if (const auto parsed = parseAnnotationColor(*color); parsed.has_value()) {
        state.color[i] = *parsed;
      }
    }
  }
  return state;
}

void ScreenshotService::persistAnnotationToolState(std::string_view key, std::string_view value) {
  (void)m_configService.setStateString(kAnnotateStateOwner, key, value);
}

void ScreenshotService::ensureAnnotationOverlay() {
  if (m_regionRenderContext == nullptr) {
    m_regionRenderContext = PanelManager::instance().renderContext();
  }
  if (m_regionRenderContext == nullptr) {
    return;
  }
  if (m_annotationOverlay == nullptr) {
    m_annotationOverlay = std::make_unique<capture::AnnotationOverlay>();
  }
  m_annotationOverlay->setIslandHost(m_islandHost);
  m_annotationOverlay->initialize(m_wayland, m_regionRenderContext);
  m_annotationOverlay->setStateSetter([this](std::string_view key, std::string_view value) {
    persistAnnotationToolState(key, value);
  });
  m_annotationOverlay->setFailureCallback([this](const std::string& message) {
    m_pendingDelivery.reset();
    notifyError(message);
  });
  m_annotationOverlay->setClosedCallback([this]() { m_pendingDelivery.reset(); });
  m_annotationOverlay->setFeedbackCallback([this](const std::string& message) { notifyError(message); });
  m_annotationOverlay->setFreezeCallback([this]() {
    if (m_annotationOverlay == nullptr || !m_annotationOverlay->isActive()) {
      return;
    }
    if (!available()) {
      notifyError("Screen capture is not available on this compositor");
      return;
    }
    // Surfaces go away before the first capture_output request, so the frame the
    // compositor copies no longer contains the ink.
    m_annotationOverlay->hideForCapture();
    m_freezeTarget = FreezeTarget::Annotation;
    beginFreezeCapture();
  });
  m_annotationOverlay->setRecordCallback([this](bool monitor) {
    if (ScreenRecorder::instance().active()) {
      ScreenRecorder::instance().stop();
      return;
    }
    if (!m_annotationOverlay || !m_annotationOverlay->isActive())
      return;
    m_annotationOverlay->cancel();
    const auto result = beginRecording(monitor);
    if (result.starts_with("error:")) {
      m_notifications.addInternal("Noctalia", "Recording not started", result, Urgency::Critical);
    }
  });
  m_annotationOverlay->setCaptureRegionCallback([this]() {
    if (m_annotationOverlay == nullptr || !m_annotationOverlay->isActive()) {
      return;
    }
    if (!available()) {
      notifyError("Screen capture is not available on this compositor");
      return;
    }
    OutputOptions options = outputOptionsFromConfig(m_configService.config());
    options.annotate = true;
    if (m_annotationOverlay->mode() != capture::AnnotationMode::Live) {
      options.showCursor = m_annotationOverlay->cursorVisible();
    }
    m_annotationOverlay->cancel();
    beginRegionCapture(*m_regionRenderContext, options);
  });
  m_annotationOverlay->setExportCallback([this](ScreencopyImage image, capture::AnnotationExport action) {
    if (action == capture::AnnotationExport::Done) {
      if (!m_pendingDelivery.has_value()) {
        return;
      }
      OutputOptions options = m_pendingDelivery->options;
      options.annotate = false;
      const auto destPath = m_pendingDelivery->destPath;
      m_pendingDelivery.reset();
      finishDelivery(std::move(image), options, destPath);
      return;
    }

    OutputOptions options = m_pendingDelivery.has_value() ? m_pendingDelivery->options : m_regionOutputOptions;
    options.copyToClipboard = action == capture::AnnotationExport::Copy;
    options.saveToFile = action == capture::AnnotationExport::Save;
    options.pipeToCommand = false;
    options.annotate = false;
    const std::optional<std::filesystem::path> destPath =
        options.saveToFile ? std::optional(makeScreenshotPath(options, "annotated")) : std::nullopt;
    const bool delivered = finishDelivery(std::move(image), options, destPath);
    if (delivered
        && ((action == capture::AnnotationExport::Copy && m_configService.config().shell.screenshot.closeOnCopy)
            || (action == capture::AnnotationExport::Save && m_configService.config().shell.screenshot.closeOnSave))) {
      DeferredCall::callLater([this]() { m_annotationOverlay->cancel(); });
    }
  });
}

void ScreenshotService::beginAnnotation(RenderContext& renderContext, const OutputOptions& options, bool freezeFirst) {
  cancelMenuCapture();
  m_regionRenderContext = &renderContext;
  m_regionOutputOptions = options;
  m_regionFullscreenPick = false;
  m_pendingDelivery.reset();

  ensureAnnotationOverlay();
  if (m_annotationOverlay == nullptr) {
    notifyError("Render context unavailable");
    return;
  }
  m_annotationOverlay->setToolState(loadAnnotationToolState());
  m_annotationOverlay->setCursorVisible(options.showCursor);

  if (freezeFirst) {
    m_freezeTarget = FreezeTarget::Annotation;
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  m_annotationOverlay->setFrozenScreenshots({});
  m_annotationOverlay->begin();
}

std::expected<void, std::string> ScreenshotService::beginImageFileAnnotation(
    RenderContext& renderContext, const std::string& path, const OutputOptions& options
) {
  if (preferredCaptureOutput() == nullptr) {
    return std::unexpected("no usable output for the annotation editor");
  }
  auto image = loadImageForAnnotation(path);
  if (!image) {
    return std::unexpected(image.error());
  }

  m_regionRenderContext = &renderContext;
  m_regionOutputOptions = options;
  m_regionFullscreenPick = false;
  m_frozenScreenshots.clear();

  // Done writes a new screenshot file rather than overwriting the source image.
  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, "annotated")) : std::nullopt;
  beginImageAnnotation(std::move(*image), options, destPath);
  return {};
}

void ScreenshotService::beginImageAnnotation(
    capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
) {
  ensureAnnotationOverlay();
  if (m_annotationOverlay == nullptr) {
    kLog.warn("annotate requested but no render context is available; delivering unannotated");
    finishDelivery(std::move(image.image), options, std::move(destPath));
    return;
  }
  m_pendingDelivery = PendingDelivery{.options = options, .destPath = std::move(destPath)};
  m_annotationOverlay->setToolState(loadAnnotationToolState());
  m_annotationOverlay->setCursorVisible(options.showCursor);
  m_annotationOverlay->beginImage(std::move(image), preferredCaptureOutput());
}

void ScreenshotService::deliverFrozenGlobalRegion(LogicalRect globalRegion, const OutputOptions& options) {
  const auto targets = intersectGlobalRegion(m_wayland, globalRegion);
  if (targets.empty()) {
    notifyError("Failed to crop frozen screenshot");
    m_frozenScreenshots.clear();
    return;
  }

  std::vector<GlobalRegionPiece> pieces;
  pieces.reserve(targets.size());
  for (const auto& target : targets) {
    auto* frozen = findFrozenScreenshot(m_frozenScreenshots, target.output);
    const auto* out = findOutput(m_wayland, target.output);
    if (frozen == nullptr || out == nullptr) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    auto cropped = cropScreenshotImage(frozen->image, out->logicalWidth, out->logicalHeight, target.localRegion);
    if (!cropped.has_value()) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    pieces.push_back(
        GlobalRegionPiece{
            .output = out,
            .localRegion = target.localRegion,
            .image = std::move(*cropped),
        }
    );
  }

  m_frozenScreenshots.clear();
  auto composed = composeGlobalRegion(globalRegion, std::move(pieces));
  if (!composed.has_value()) {
    notifyError("Failed to crop frozen screenshot");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*composed), options, destPath);
}

void ScreenshotService::captureGlobalRegion(LogicalRect globalRegion, const OutputOptions& options) {
  cancelAllOutputsBatch();
  cancelGlobalRegionBatch();
  m_captureQueue.clear();
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  const auto targets = intersectGlobalRegion(m_wayland, globalRegion);
  if (targets.empty()) {
    notifyError("No outputs available");
    return;
  }
  if (targets.size() == 1) {
    captureOutput(targets.front().output, targets.front().localRegion, "region", options);
    return;
  }

  std::vector<GlobalRegionCaptureTarget> batchTargets;
  batchTargets.reserve(targets.size());
  for (const auto& target : targets) {
    batchTargets.push_back(
        GlobalRegionCaptureTarget{
            .output = target.output,
            .localRegion = target.localRegion,
        }
    );
  }

  m_globalRegionBatch = std::make_unique<GlobalRegionBatch>(GlobalRegionBatch{
      .options = options,
      .globalRegion = globalRegion,
      .targets = std::move(batchTargets),
      .pieces = {},
      .next = 0,
  });
  startNextGlobalRegionCapture();
}

void ScreenshotService::startNextGlobalRegionCapture() {
  if (!m_globalRegionBatch) {
    return;
  }

  auto& batch = *m_globalRegionBatch;
  while (batch.next < batch.targets.size() && batch.targets[batch.next].output == nullptr) {
    ++batch.next;
  }
  if (batch.next >= batch.targets.size()) {
    finishGlobalRegionBatch();
    return;
  }

  const GlobalRegionCaptureTarget target = batch.targets[batch.next];
  ++batch.next;
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  m_capture.capture(
      target.output, target.localRegion, batch.options.showCursor, batch.options.annotate,
      [this, output = target.output,
       localRegion = target.localRegion](std::optional<capture::ScreenshotImage> image, const std::string& error) {
        onGlobalRegionFrameCaptured(output, localRegion, std::move(image), error);
      }
  );
}

void ScreenshotService::onGlobalRegionFrameCaptured(
    wl_output* output, LogicalRect localRegion, std::optional<capture::ScreenshotImage> image, const std::string& error
) {
  if (!m_globalRegionBatch) {
    return;
  }
  if (!error.empty() || !image.has_value()) {
    kLog.warn("region screenshot failed: {}", error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    cancelGlobalRegionBatch();
    return;
  }

  m_globalRegionBatch->pieces.push_back(
      GlobalRegionBatch::Piece{
          .output = output,
          .localRegion = localRegion,
          .image = std::move(*image),
      }
  );
  DeferredCall::callLater([this]() { startNextGlobalRegionCapture(); });
}

void ScreenshotService::finishGlobalRegionBatch() {
  if (!m_globalRegionBatch) {
    return;
  }

  GlobalRegionBatch batch = std::move(*m_globalRegionBatch);
  m_globalRegionBatch.reset();
  if (batch.pieces.empty()) {
    notifyError("Screenshot failed");
    return;
  }

  std::vector<GlobalRegionPiece> pieces;
  pieces.reserve(batch.pieces.size());
  for (auto& piece : batch.pieces) {
    const auto* out = findOutput(m_wayland, piece.output);
    if (out == nullptr) {
      notifyError("Failed to combine screenshots");
      return;
    }
    pieces.push_back(
        GlobalRegionPiece{
            .output = out,
            .localRegion = piece.localRegion,
            .image = std::move(piece.image),
        }
    );
  }

  auto composed = composeGlobalRegion(batch.globalRegion, std::move(pieces));
  if (!composed.has_value()) {
    notifyError("Failed to combine screenshots");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(batch.options) ? std::optional(makeScreenshotPath(batch.options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*composed), batch.options, destPath);
}

void ScreenshotService::cancelGlobalRegionBatch() { m_globalRegionBatch.reset(); }

void ScreenshotService::deliverFrozenRegion(LogicalRect region, wl_output* output, const OutputOptions& options) {
  auto* frozen = findFrozenScreenshot(m_frozenScreenshots, output);
  const auto* out = findOutput(m_wayland, output);
  if (frozen == nullptr || out == nullptr) {
    notifyError("Failed to crop frozen screenshot");
    m_frozenScreenshots.clear();
    return;
  }

  auto cropped = cropScreenshotImage(frozen->image, out->logicalWidth, out->logicalHeight, region);
  m_frozenScreenshots.clear();
  if (!cropped.has_value()) {
    notifyError("Failed to crop frozen screenshot");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, "region")) : std::nullopt;
  deliverCaptureResult(std::move(*cropped), options, destPath);
}

void ScreenshotService::completeFullscreenSelection(wl_output* output, const OutputOptions& options) {
  if (output == nullptr) {
    m_frozenScreenshots.clear();
    return;
  }
  if (options.freezeScreen && !m_frozenScreenshots.empty()) {
    const auto* out = findOutput(m_wayland, output);
    if (out == nullptr) {
      notifyError("Failed to crop frozen screenshot");
      m_frozenScreenshots.clear();
      return;
    }
    deliverFrozenRegion(
        LogicalRect{
            .x = 0,
            .y = 0,
            .width = out->logicalWidth,
            .height = out->logicalHeight,
        },
        output, options
    );
    return;
  }
  m_frozenScreenshots.clear();
  captureOutput(output, std::nullopt, "screenshot", options);
}

void ScreenshotService::captureOutput(
    wl_output* output, std::optional<LogicalRect> region, const std::string& labelBase, const OutputOptions& options,
    int pathSuffix
) {
  if (output == nullptr) {
    notifyError("No output for capture");
    return;
  }

  PendingCapture pending{
      .output = output,
      .region = region,
      .outputOptions = options,
      .destPath = needsScreenshotPath(options) ? std::optional(makeScreenshotPath(options, labelBase, pathSuffix))
                                               : std::nullopt,
  };
  if (m_capture.busy()) {
    m_captureQueue.push_back(std::move(pending));
    return;
  }

  m_capture.capture(
      pending.output, pending.region, pending.outputOptions.showCursor, pending.outputOptions.annotate,
      [this, options = pending.outputOptions,
       destPath = pending.destPath](std::optional<capture::ScreenshotImage> image, const std::string& error) {
        onCaptureComplete(std::move(image), error, options, destPath);
      }
  );
}

void ScreenshotService::startNextQueuedCapture() {
  if (m_captureQueue.empty() || m_capture.busy()) {
    return;
  }
  DeferredCall::callLater([this]() {
    if (m_captureQueue.empty() || m_capture.busy()) {
      return;
    }
    PendingCapture pending = std::move(m_captureQueue.front());
    m_captureQueue.erase(m_captureQueue.begin());
    m_capture.capture(
        pending.output, pending.region, pending.outputOptions.showCursor, pending.outputOptions.annotate,
        [this, options = pending.outputOptions,
         destPath = pending.destPath](std::optional<capture::ScreenshotImage> image, const std::string& error) {
          onCaptureComplete(std::move(image), error, options, destPath);
        }
    );
  });
}

void ScreenshotService::captureAllOutputs(const OutputOptions& options) {
  if (m_annotationOverlay != nullptr && m_annotationOverlay->isActive()) {
    notifyError("An annotation overlay is already active");
    return;
  }
  cancelAllOutputsBatch();
  cancelGlobalRegionBatch();
  m_captureQueue.clear();
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  std::vector<AllOutputCaptureTarget> targets;
  int index = 0;
  for (const auto& output : m_wayland.outputs()) {
    if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      continue;
    }
    ++index;
    AllOutputCaptureTarget target{
        .output = output.output,
        .label = output.connectorName.empty() ? ("monitor-" + std::to_string(index)) : output.connectorName,
    };
    targets.push_back(std::move(target));
  }
  if (targets.empty()) {
    notifyError("No outputs available");
    return;
  }
  playCaptureSound();
  if (targets.size() == 1) {
    captureOutput(targets.front().output, std::nullopt, targets.front().label, options);
    return;
  }

  m_allOutputsBatch = std::make_unique<AllOutputsBatch>(AllOutputsBatch{
      .options = options,
      .targets = std::move(targets),
      .frames = {},
      .next = 0,
  });
  startNextAllOutputsCapture();
}

void ScreenshotService::startNextAllOutputsCapture() {
  if (!m_allOutputsBatch) {
    return;
  }

  auto& batch = *m_allOutputsBatch;
  while (batch.next < batch.targets.size() && batch.targets[batch.next].output == nullptr) {
    ++batch.next;
  }
  if (batch.next >= batch.targets.size()) {
    finishAllOutputsBatch();
    return;
  }

  const AllOutputCaptureTarget target = batch.targets[batch.next];
  ++batch.next;
  if (m_capture.busy()) {
    m_capture.cancelInFlight();
  }

  m_capture.capture(
      target.output, std::nullopt, batch.options.showCursor, batch.options.annotate,
      [this, output = target.output,
       label = target.label](std::optional<capture::ScreenshotImage> image, const std::string& error) {
        onAllOutputsFrameCaptured(output, label, std::move(image), error);
      }
  );
}

void ScreenshotService::onAllOutputsFrameCaptured(
    wl_output* output, const std::string& label, std::optional<capture::ScreenshotImage> image, const std::string& error
) {
  if (!m_allOutputsBatch) {
    return;
  }
  if (!error.empty() || !image.has_value()) {
    kLog.warn("screenshot failed for {}: {}", label, error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    cancelAllOutputsBatch();
    return;
  }

  m_allOutputsBatch->frames.push_back(capture::FrozenScreenshot{.output = output, .image = std::move(*image)});
  DeferredCall::callLater([this]() { startNextAllOutputsCapture(); });
}

void ScreenshotService::finishAllOutputsBatch() {
  if (!m_allOutputsBatch) {
    return;
  }

  AllOutputsBatch batch = std::move(*m_allOutputsBatch);
  m_allOutputsBatch.reset();
  if (batch.frames.empty()) {
    notifyError("Screenshot failed");
    return;
  }

  std::vector<CapturedOutputFrame> frames;
  frames.reserve(batch.frames.size());
  for (auto& frame : batch.frames) {
    const auto* out = findOutput(m_wayland, frame.output);
    if (out == nullptr) {
      notifyError("Failed to combine screenshots");
      return;
    }
    frames.push_back(CapturedOutputFrame{.image = std::move(frame.image), .output = out});
  }

  auto stitched = stitchOutputFrames(std::move(frames));
  if (!stitched.has_value()) {
    notifyError("Failed to combine screenshots");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      needsScreenshotPath(batch.options) ? std::optional(makeScreenshotPath(batch.options, "desktop")) : std::nullopt;
  deliverCaptureResult(std::move(*stitched), batch.options, destPath);
}

void ScreenshotService::cancelAllOutputsBatch() {
  m_allOutputsBatch.reset();
  cancelGlobalRegionBatch();
}

void ScreenshotService::deliverCaptureResult(
    capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
) {
  if (options.annotate && image.image.width > 0 && image.image.height > 0) {
    beginImageAnnotation(std::move(image), options, std::move(destPath));
    return;
  }
  finishDelivery(std::move(image.image), options, std::move(destPath));
}

bool ScreenshotService::finishDelivery(
    ScreencopyImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
) {
  std::string encodeError;
  std::vector<std::uint8_t> png = encodePng(image.rgba.data(), image.width, image.height, &encodeError);
  if (png.empty()) {
    kLog.warn("screenshot encode failed: {}", encodeError);
    notifyError(encodeError.empty() ? "Failed to encode screenshot" : encodeError);
    return false;
  }

  if (options.extractText)
    return extractText(png);

  bool delivered = false;
  std::string failureMessage;

  if (destPath.has_value()) {
    std::error_code ec;
    std::filesystem::create_directories(destPath->parent_path(), ec);
    if (ec) {
      kLog.warn("screenshot directory create failed: {}", destPath->parent_path().string());
    }
  }

  if (options.saveToFile && destPath.has_value()) {
    std::ofstream out(*destPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
      kLog.warn("screenshot write failed: {}", destPath->string());
      failureMessage = "Failed to save screenshot";
    } else {
      notifySaved(*destPath, image);
      delivered = true;
    }
  }

  if (options.copyToClipboard) {
    if (m_clipboard == nullptr || !m_clipboard->isAvailable()) {
      kLog.warn("screenshot clipboard copy skipped: clipboard unavailable");
      if (failureMessage.empty()) {
        failureMessage = "Clipboard is not available";
      }
    } else if (m_clipboard->copyImagePng(png)) {
      delivered = true;
    } else {
      kLog.warn("screenshot clipboard copy failed");
      if (failureMessage.empty()) {
        failureMessage = "Failed to copy screenshot to clipboard";
      }
    }
  }

  if (options.pipeToCommand && !options.pipeCommand.empty()) {
    pipePngToCommandAsync(options.pipeCommand, png, destPath);
    delivered = true;
  }

  if (!delivered) {
    notifyError(failureMessage.empty() ? "No screenshot output enabled" : failureMessage);
  }
  return delivered;
}

void ScreenshotService::cancelTextCapture() {
  if (m_textCancel) {
    *m_textCancel = true;
    m_textCancel.reset();
  }
  if (m_regionOutputOptions.extractText)
    cancelRegionCapture();
}

bool ScreenshotService::extractText(const std::vector<std::uint8_t>& png) {
  if (textCaptureAllowed && !textCaptureAllowed())
    return false;
  // mkstemp creates a private file; only the recogniser receives its path. Neither image nor
  // recognised text goes into screenshot history, notifications or logs.
  char pattern[] = "/tmp/noctalia-text-XXXXXX";
  const int fd = mkstemp(pattern);
  if (fd < 0) {
    notifyError(i18n::tr("utilities.text.failed"));
    return false;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  const std::string path(pattern);
  std::size_t offset = 0;
  while (offset < png.size()) {
    const auto count = write(fd, png.data() + offset, png.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    offset += static_cast<std::size_t>(count);
  }
  close(fd);
  if (offset != png.size()) {
    unlink(path.c_str());
    notifyError(i18n::tr("utilities.text.failed"));
    return false;
  }
  const auto& config = m_configService.config().shell.screenshot;
  std::vector<std::string> args{"tesseract", path, "stdout", "-l", config.textLanguages, "--psm", "11"};
  if (!config.textDataDirectory.empty()) {
    args.emplace_back("--tessdata-dir");
    args.push_back(config.textDataDirectory);
  }
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_textCancel = cancel;
  const std::weak_ptr<int> lifetime = m_textLifetime;
  if (textCaptureFeedback)
    textCaptureFeedback(i18n::tr("utilities.text.reading"));
  const bool launched = process::runAsync(
      args,
      {.onExit =
           [this, lifetime, cancel, path](process::RunResult result) {
             unlink(path.c_str());
             DeferredCall::callLater([this, lifetime, cancel, result = std::move(result)] {
               if (lifetime.expired() || *cancel || m_textCancel != cancel)
                 return;
               m_textCancel.reset();
               if (textCaptureAllowed && !textCaptureAllowed())
                 return;
               std::string key = "utilities.text.failed";
               if (result && !result.outTruncated) {
                 const auto text = StringUtils::trim(result.out);
                 if (text.empty())
                   key = "utilities.text.empty";
                 else if (m_clipboard && m_clipboard->copyText(text))
                   key = "utilities.text.copied";
                 else
                   key = "utilities.text.copy-failed";
               }
               if (textCaptureFeedback)
                 textCaptureFeedback(i18n::tr(key));
             });
           }},
      {.timeout = std::chrono::seconds(30), .maxOutputBytes = 1024 * 1024, .cancel = cancel}
  );
  if (!launched) {
    m_textCancel.reset();
    unlink(path.c_str());
    if (textCaptureFeedback)
      textCaptureFeedback(i18n::tr("utilities.text.failed"));
  }
  return launched;
}

void ScreenshotService::onCaptureComplete(
    std::optional<capture::ScreenshotImage> image, const std::string& error, OutputOptions options,
    std::optional<std::filesystem::path> destPath
) {
  if (!error.empty() || !image.has_value()) {
    kLog.warn("screenshot failed: {}", error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    startNextQueuedCapture();
    return;
  }

  deliverCaptureResult(std::move(*image), options, std::move(destPath));
  startNextQueuedCapture();
}

std::filesystem::path ScreenshotService::outputDirectory(const OutputOptions& options) const {
  if (options.directory.empty()) {
    return FileUtils::defaultPicturesDirectory();
  }
  return FileUtils::expandUserPath(options.directory);
}

std::filesystem::path
ScreenshotService::makeScreenshotPath(const OutputOptions& options, const std::string& labelBase, int suffix) const {
  const auto dir = outputDirectory(options);
  const std::string stem = formatFilenameStem(options.filenamePattern, labelBase, suffix);
  return dir / (stem + ".png");
}

void ScreenshotService::notifySaved(const std::filesystem::path& path, const ScreencopyImage& image) {
  NotificationRequest request;
  request.appName = "Noctalia";
  request.summary = i18n::tr("notifications.internal.screenshot-saved");
  request.body = path.filename().string();
  request.origin = NotificationOrigin::Internal;
  request.category = std::string(kScreenshotNotificationCategory);
  request.actions = {
      "default", i18n::tr("notifications.actions.open"),
      "markup",  i18n::tr("notifications.internal.screenshot-markup"),
      "folder",  i18n::tr("notifications.internal.screenshot-show-in-folder"),
  };
  // A small copy for the notification, like the floating thumbnail on macOS.
  ScreencopyImage thumbnail = image;
  const int width = std::min(thumbnail.width, kScreenshotThumbnailWidth);
  const int height = std::max(1, thumbnail.height * width / std::max(1, thumbnail.width));
  if (resampleRgbaImage(thumbnail, width, height)) {
    request.imageData = NotificationImageData{
        .width = thumbnail.width,
        .height = thumbnail.height,
        .rowStride = thumbnail.width * 4,
        .hasAlpha = true,
        .bitsPerSample = 8,
        .channels = 4,
        .data = std::move(thumbnail.rgba),
    };
  }
  const std::uint32_t id = m_notifications.addOrReplace(std::move(request));
  if (id == 0) {
    return;
  }
  m_savedNotifications.emplace_back(id, path);
  if (m_savedNotifications.size() > 8) {
    m_savedNotifications.erase(m_savedNotifications.begin());
  }
}

void ScreenshotService::notifyRecordingSaved(const ScreenRecorder::Result& result) {
  const auto seconds = result.duration.count();
  const std::string duration = seconds >= 3600
      ? std::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60)
      : std::format("{}:{:02}", seconds / 60, seconds % 60);
  NotificationRequest request;
  request.appName = "Noctalia";
  request.summary = i18n::tr("notifications.internal.recording-saved");
  request.body = i18n::tr(
      "notifications.internal.recording-details", "duration", duration, "size",
      launcher_util::formatByteSize(result.bytes)
  );
  request.origin = NotificationOrigin::Internal;
  request.category = std::string(kRecordingNotificationCategory);
  request.timeout = 5000;
  request.actions = {
      "default",
      i18n::tr("notifications.internal.recording-play"),
      "folder",
      i18n::tr("notifications.internal.screenshot-show-in-folder"),
  };
  const auto id = m_notifications.addOrReplace(std::move(request));
  if (!id)
    return;
  m_savedNotifications.emplace_back(id, result.path);
  if (m_savedNotifications.size() > 8)
    m_savedNotifications.erase(m_savedNotifications.begin());

  // Publish the usable result immediately. A bounded decoder job fills the preview later.
  if (m_recordingPreviewCancel)
    *m_recordingPreviewCancel = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_recordingPreviewCancel = cancel;
  const std::weak_ptr<int> lifetime = m_recordingPreviewLifetime;
  constexpr int width = 480, height = 270;
  constexpr std::size_t bytes = width * height * 4;
  const bool launched = process::runAsync(
      std::vector<std::string>{
          "ffmpeg",
          "-nostdin",
          "-v",
          "error",
          "-threads",
          "1",
          "-i",
          result.path.string(),
          "-map",
          "0:v:0",
          "-frames:v",
          "1",
          "-an",
          "-sn",
          "-filter_threads",
          "1",
          "-vf",
          "scale=480:270:force_original_aspect_ratio=decrease,pad=480:270:(ow-iw)/2:(oh-ih)/2,setsar=1",
          "-threads",
          "1",
          "-f",
          "rawvideo",
          "-pix_fmt",
          "rgba",
          "pipe:1",
      },
      {.onExit =
           [this, lifetime, cancel, id](process::RunResult preview) {
             if (!preview || preview.outTruncated || preview.out.size() != bytes || *cancel)
               return;
             NotificationImageData image{
                 .width = width,
                 .height = height,
                 .rowStride = width * 4,
                 .data = std::vector<std::uint8_t>(preview.out.begin(), preview.out.end()),
             };
             DeferredCall::callLater([this, lifetime, cancel, id, image = std::move(image)]() mutable {
               if (lifetime.expired() || *cancel)
                 return;
               (void)m_notifications.updateImage(id, std::move(image));
             });
           }},
      {.timeout = std::chrono::seconds(3), .maxOutputBytes = bytes, .cancel = cancel}
  );
  if (!launched)
    m_recordingPreviewCancel.reset();
}

void ScreenshotService::onSavedNotificationAction(
    std::uint32_t id, const std::string& actionKey, const std::string& activationToken
) {
  const auto it = std::ranges::find(m_savedNotifications, id, &std::pair<std::uint32_t, std::filesystem::path>::first);
  if (it == m_savedNotifications.end()) {
    return;
  }
  const std::filesystem::path path = it->second;
  if (actionKey == "markup") {
    auto* renderContext = PanelManager::instance().renderContext();
    if (renderContext == nullptr) {
      return;
    }
    if (const auto started =
            beginImageFileAnnotation(*renderContext, path.string(), outputOptionsFromConfig(m_configService.config()));
        !started) {
      notifyError(started.error());
    }
    return;
  }
  const std::string target = actionKey == "folder" ? path.parent_path().string() : path.string();
  if (!process::runAsync(std::vector<std::string>{"xdg-open", target}, activationToken)) {
    kLog.warn("failed to open {}", target);
  }
}

void ScreenshotService::notifyError(const std::string& message) {
  m_notifications.addInternal("Noctalia", "Screenshot failed", message, Urgency::Critical);
}

void ScreenshotService::releaseIslandCapture() {
  if (m_annotationOverlay && m_annotationOverlay->isActive())
    m_annotationOverlay->cancel();
}

void ScreenshotService::setSoundPlayer(SoundPlayer* soundPlayer) { m_soundPlayer = soundPlayer; }

void ScreenshotService::playCaptureSound() {
  if (m_soundPlayer != nullptr) {
    m_soundPlayer->play("screen-capture");
  }
}
