#pragma once

#include "capture/annotation_overlay.h"
#include "capture/capture_options.h"
#include "capture/screen_recorder.h"
#include "capture/screenshot_capture.h"
#include "capture/screenshot_region_overlay.h"
#include "capture/toplevel_thumbnail_capture.h"
#include "core/timer_manager.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class CompositorPlatform;
class ConfigService;
class IpcService;
class Island;
class NotificationManager;
struct Config;
class RenderContext;
class WaylandConnection;
class SoundPlayer;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

class ScreenshotService {
public:
  struct OutputOptions {
    bool saveToFile = true;
    bool copyToClipboard = false;
    bool pipeToCommand = false;
    bool freezeScreen = false;
    bool confirmRegion = false;
    bool rememberLastRegion = false;
    bool showCursor = false;
    bool annotate = false;
    bool extractText = false;
    std::string pipeCommand;
    std::string directory;
    std::string filenamePattern;
  };

  ScreenshotService(
      WaylandConnection& wayland, CompositorPlatform& platform, ConfigService& configService,
      NotificationManager& notifications, ClipboardService* clipboard = nullptr
  );
  ~ScreenshotService();

  [[nodiscard]] bool available() const noexcept;

  void captureFullscreen(const OutputOptions& options, wl_output* output = nullptr);
  void captureFullscreenInteractive(RenderContext& renderContext, const OutputOptions& options);
  void beginRegionCapture(RenderContext& renderContext, const OutputOptions& options);
  void beginFullscreenCapture(RenderContext& renderContext, const OutputOptions& options);
  // freezeFirst selects the frozen annotator (screenshot-annotate); otherwise the overlay
  // starts transparent over the running desktop (annotate).
  void beginAnnotation(RenderContext& renderContext, const OutputOptions& options, bool freezeFirst);
  // Opens the annotator on an image decoded from disk, with no screencopy involved.
  // Returns the reason when the file cannot be decoded or no output can host the editor.
  [[nodiscard]] std::expected<void, std::string>
  beginImageFileAnnotation(RenderContext& renderContext, const std::string& path, const OutputOptions& options);
  [[nodiscard]] bool overlayBusy() const noexcept;

  void onOutputChange();

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] static OutputOptions outputOptionsFromConfig(const Config& config);

  void registerIpc(IpcService& ipc, const ConfigService& configService);

  void setIslandHost(Island* island) { m_islandHost = island; }
  void releaseIslandCapture();
  void setSoundPlayer(SoundPlayer* soundPlayer);
  std::function<void(const std::string&)> textCaptureFeedback;
  std::function<bool()> textCaptureAllowed;
  void cancelTextCapture();
  // Menu captures select a target first, then count down before taking a fresh frame.
  std::string beginMenuCapture(capture::LaunchOptions options);
  void cancelMenuCapture();
  std::function<void(bool recording, int remaining, const std::string& output)> captureCountdown;

private:
  struct MenuCapture {
    capture::LaunchOptions launch;
    OutputOptions outputOptions;
    LogicalRect region;
    std::string output;
    std::string windowId;
    std::chrono::steady_clock::time_point deadline;
    int lastRemaining = -1;
  };
  std::optional<capture::LaunchOptions> m_menuSelection;
  std::optional<MenuCapture> m_menuCapture;
  Timer m_menuTimer;
  void finishMenuSelection(LogicalRect region, wl_output* output, capture::LaunchOptions options);
  void tickMenuCapture();
  void runMenuCapture();
  void refreshWindowTargets();
  void requestWindowTargets(std::function<void(std::optional<std::vector<capture::WindowTarget>>)> callback);
  Timer m_windowRefresh;
  std::shared_ptr<std::atomic<bool>> m_windowCancel;
  std::shared_ptr<int> m_windowLifetime = std::make_shared<int>(0);
  struct PendingCapture {
    wl_output* output = nullptr;
    std::optional<LogicalRect> region;
    OutputOptions outputOptions{};
    std::optional<std::filesystem::path> destPath;
  };

  struct AllOutputCaptureTarget {
    wl_output* output = nullptr;
    std::string label;
  };

  struct AllOutputsBatch {
    OutputOptions options{};
    std::vector<AllOutputCaptureTarget> targets;
    std::vector<capture::FrozenScreenshot> frames;
    std::size_t next = 0;
  };

  struct GlobalRegionCaptureTarget {
    wl_output* output = nullptr;
    LogicalRect localRegion{};
  };

  struct GlobalRegionBatch {
    OutputOptions options{};
    LogicalRect globalRegion{};
    std::vector<GlobalRegionCaptureTarget> targets;
    struct Piece {
      wl_output* output = nullptr;
      LogicalRect localRegion{};
      capture::ScreenshotImage image;
    };
    std::vector<Piece> pieces;
    std::size_t next = 0;
  };

  enum class FreezeTarget : std::uint8_t { Region, Annotation };

  struct FreezeRequest {
    wl_output* output = nullptr;
  };

  // Delivery policy captured before the annotator opened, applied when the user hits Done.
  struct PendingDelivery {
    OutputOptions options{};
    std::optional<std::filesystem::path> destPath;
  };

  void captureOutput(
      wl_output* output, std::optional<LogicalRect> region, const std::string& labelBase, const OutputOptions& options,
      int pathSuffix = 0
  );
  std::string beginRecording(bool monitor);
  void ensureRegionOverlay();
  void startRegionOverlay(RenderContext& renderContext);
  void startFullscreenOverlay(RenderContext& renderContext);
  void beginFreezeCapture();
  void startNextFreezeCapture();
  void
  onFreezeFrameCaptured(FreezeRequest request, std::optional<capture::ScreenshotImage> image, const std::string& error);
  void finishFreezeCapture();
  void abortFreezeCapture(const std::string& message);
  void cancelRegionCapture();
  void ensureAnnotationOverlay();
  void beginImageAnnotation(
      capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
  );
  [[nodiscard]] capture::AnnotationToolState loadAnnotationToolState() const;
  void persistAnnotationToolState(std::string_view key, std::string_view value);
  void deliverFrozenRegion(LogicalRect region, wl_output* output, const OutputOptions& options);
  void deliverFrozenGlobalRegion(LogicalRect globalRegion, const OutputOptions& options);
  void captureGlobalRegion(LogicalRect globalRegion, const OutputOptions& options);
  void completeFullscreenSelection(wl_output* output, const OutputOptions& options);
  void startNextGlobalRegionCapture();
  void onGlobalRegionFrameCaptured(
      wl_output* output, LogicalRect localRegion, std::optional<capture::ScreenshotImage> image,
      const std::string& error
  );
  void finishGlobalRegionBatch();
  void cancelGlobalRegionBatch();
  void startNextQueuedCapture();
  void captureAllOutputs(const OutputOptions& options);
  void startNextAllOutputsCapture();
  void onAllOutputsFrameCaptured(
      wl_output* output, const std::string& label, std::optional<capture::ScreenshotImage> image,
      const std::string& error
  );
  void finishAllOutputsBatch();
  void cancelAllOutputsBatch();
  void deliverCaptureResult(
      capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
  );
  bool
  finishDelivery(ScreencopyImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath);
  void onCaptureComplete(
      std::optional<capture::ScreenshotImage> image, const std::string& error, OutputOptions options,
      std::optional<std::filesystem::path> destPath
  );
  [[nodiscard]] wl_output* preferredCaptureOutput() const;
  [[nodiscard]] std::filesystem::path outputDirectory(const OutputOptions& options) const;
  [[nodiscard]] std::filesystem::path
  makeScreenshotPath(const OutputOptions& options, const std::string& labelBase, int suffix = 0) const;
  // Posts "Screenshot saved" with a thumbnail and Open, Markup and Show in Folder actions.
  void notifySaved(const std::filesystem::path& path, const ScreencopyImage& image);
  void notifyRecordingSaved(const ScreenRecorder::Result& result);
  void onSavedNotificationAction(std::uint32_t id, const std::string& actionKey, const std::string& activationToken);
  void notifyError(const std::string& message);
  void rememberRegion(const LogicalRect& region);
  [[nodiscard]] std::optional<LogicalRect> loadRememberedRegion() const;
  void playCaptureSound();
  bool extractText(const std::vector<std::uint8_t>& png);
  std::shared_ptr<std::atomic<bool>> m_textCancel;
  std::shared_ptr<int> m_textLifetime = std::make_shared<int>(0);
  std::shared_ptr<std::atomic<bool>> m_recordingPreviewCancel;
  std::shared_ptr<int> m_recordingPreviewLifetime = std::make_shared<int>(0);

  WaylandConnection& m_wayland;
  CompositorPlatform& m_platform;
  NotificationManager& m_notifications;
  ConfigService& m_configService;
  ClipboardService* m_clipboard = nullptr;
  ScreenshotCapture m_capture;
  ToplevelThumbnailCapture m_windowCapture;
  std::unique_ptr<capture::ScreenshotRegionOverlay> m_regionOverlay;
  std::vector<PendingCapture> m_captureQueue;
  std::unique_ptr<AllOutputsBatch> m_allOutputsBatch;
  std::unique_ptr<GlobalRegionBatch> m_globalRegionBatch;
  OutputOptions m_regionOutputOptions{};
  RenderContext* m_regionRenderContext = nullptr;
  bool m_regionFullscreenPick = false;
  bool m_recordSelection = false;
  std::vector<capture::FrozenScreenshot> m_frozenScreenshots;
  std::unique_ptr<capture::AnnotationOverlay> m_annotationOverlay;
  std::vector<FreezeRequest> m_pendingFreezeCaptures;
  std::optional<PendingDelivery> m_pendingDelivery;
  FreezeTarget m_freezeTarget = FreezeTarget::Region;
  bool m_freezeCaptureActive = false;
  SoundPlayer* m_soundPlayer = nullptr;
  Island* m_islandHost = nullptr;
  // Recent capture notifications and the files their actions open.
  std::vector<std::pair<std::uint32_t, std::filesystem::path>> m_savedNotifications;
};
