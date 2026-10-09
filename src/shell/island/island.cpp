#include "shell/island/island.h"

#include "calendar/calendar_service.h"
#include "capture/screen_recorder.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/downloads/download_progress_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_level_monitor.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/animation/motion_service.h"
#include "render/core/image_file_loader.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_state_store.h"
#include "shell/bar/bar_services.h"
#include "shell/bar/bar_visibility.h"
#include "shell/bar/widget_factory.h"
#include "shell/island/island_activity.h"
#include "shell/island/island_battery.h"
#include "shell/island/island_capture_glow.h"
#include "shell/island/island_connection.h"
#include "shell/island/island_progress_outline.h"
#include "shell/island/island_state.h"
#include "shell/island/island_style.h"
#include "shell/island/island_timer.h"
#include "shell/island/island_widget_host.h"
#include "shell/tooltip/tooltip_manager.h"
#include "time/time_format.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/spinner.h"
#include "ui/material.h"
#include "ui/motion.h"
#include "ui/palette.h"
#include "ui/visuals/audio_visualizer.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <ranges>
#include <span>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
  class IslandMicrophoneMeter : public ProgressBar {
  public:
    IslandMicrophoneMeter(PipeWireService& service, const AudioNode& source) {
      setProgress(0);
      if (source.muted)
        return;
      m_monitor = std::make_unique<PipeWireLevelMonitor>(service, source.name);
      m_timer.startRepeating(50ms, [this] { setProgress(m_monitor->level()); });
    }

    void stop() {
      m_timer.stop();
      m_monitor.reset();
      setProgress(0);
    }

  private:
    std::unique_ptr<PipeWireLevelMonitor> m_monitor;
    Timer m_timer;
  };

  class IslandAudioVisualizer : public AudioVisualizer {
  public:
    IslandAudioVisualizer(PipeWireSpectrum* spectrum, LayerSurface& surface)
        : m_spectrum(spectrum), m_surface(surface) {
      setCentered(true);
      setMirrored(false);
      // Quiet passages rest as dots, as Apple's Now Playing waveform does, not a dashed line.
      setRestAsDots(true);
      setValues(std::vector<float>(5, 0.0F));
      setActive(true);
    }

    ~IslandAudioVisualizer() override { setActive(false); }

    void setActive(bool active) {
      if (m_active == active)
        return;
      m_active = active;
      if (active) {
        if (m_spectrum)
          m_listener = m_spectrum->addChangeListener(5, [this] { m_surface.requestFrameTick(); });
        m_surface.requestFrameTick();
      } else if (m_spectrum && m_listener) {
        m_spectrum->removeChangeListener(m_listener);
        m_listener = 0;
      }
    }

    void onFrameTick(float deltaMs) {
      if (!m_active)
        return;
      if (m_spectrum && m_listener)
        setValues(m_spectrum->values(m_listener));
      const bool changing = !converged();
      tick(deltaMs);
      if (changing)
        m_surface.requestRedraw();
      if (!converged() || (m_spectrum && !m_spectrum->idle()))
        m_surface.requestFrameTick();
    }

  private:
    PipeWireSpectrum* m_spectrum;
    LayerSurface& m_surface;
    PipeWireSpectrum::ListenerId m_listener = 0;
    bool m_active = false;
  };
} // namespace

struct Island::Instance {
  std::vector<IslandMicrophoneMeter*> microphoneMeters;
  BarConfig barConfig;
  IslandConfig config;
  float visibility = 1;
  bool wantsVisible = true;
  AnimationManager::Id hideAnimation = 0;
  std::string workspaceId;
  bool workspacePeek = false;
  Timer peek;
  wl_output* output = nullptr;
  std::unique_ptr<LayerSurface> surface;
  // The surface spans the output's width so it never recentres sideways (Hyprland animates that),
  // but is only as tall as the capsule needs: compositor effects such as hyprglass's layer glass
  // cost in proportion to the layer's area, not the capsule's. A hosted panel gets the full height.
  std::uint32_t surfaceWidth = 0;
  std::uint32_t surfaceHeight = 0;
  // The output's logical height, which still bounds tall content (notifications, the hover view).
  float outputHeight = 0;
  std::optional<std::array<int, 4>> inputRegion;
  // The blur region last sent for glass, so unchanged frames send nothing.
  std::vector<std::array<int, 4>> blurRegion;
  AnimationManager animations;
  InputDispatcher input;
  std::unique_ptr<Node> root;
  Box* background = nullptr;
  island::ProgressOutline* progressOutline = nullptr;
  island::CaptureGlow* captureGlow = nullptr;
  // Behind the content inside the capsule: the artwork flow while media plays.
  Image* flowImage = nullptr;
  TextureHandle flowTexture{};
  bool flowShown = false;
  std::string flowArt;
  Node* content = nullptr;
  // View and status-card crossfades share one outgoing layer and one incoming fade.
  Node* outgoing = nullptr;
  float contentFade = 1.0F;
  AnimationManager::Id contentFadeAnimation = 0;
  std::uint64_t crossfadeSerial = 0;
  std::string cardPresentation;
  // How much of the outgoing content still shows (1 → 0); the incoming content waits on it so
  // two views (two clocks, say) never show at once.
  float outgoingFade = 0.0F;
  // The capsule width the outgoing content was laid out for, to keep it centred as it fades.
  float outgoingWidth = 0.0F;
  // Set when a hosted panel hands the surface back: the panel has already faded out and shrunk
  // to the capsule, so the view from before it opened (often the wide hover view) is dropped
  // rather than crossfaded out over the compact one.
  bool skipCrossfade = false;
  ScrollView* activityScroll = nullptr;
  island::View previousView = island::View::Rest;
  float scale = 1;
  float width = 160;
  float height = 64;
  float targetWidth = 160;
  float targetHeight = 64;
  AnimationManager::Id morph = 0;
  // The morph spring's current speed in points per second, carried into the next morph when
  // a size change interrupts it so the capsule redirects without a jolt.
  float widthVelocity = 0;
  float heightVelocity = 0;
  bool panelHosted = false;
  bool inside = false;
  bool hovered = false;
  bool badgeHovered = false;
  bool recentTransfersExpanded = false;
  // Split Island: other running activities in round bubbles beside the capsule, as on iPhone.
  // The first sits behind the capsule and slides out from under its right end; the second sits
  // behind the first and slides out from under that, for a third concurrent activity.
  struct SplitBubble {
    Box* bubble = nullptr;
    InputArea* area = nullptr;
    Node* content = nullptr;
    Node* outgoing = nullptr;
    float contentFade = 1;
    AnimationManager::Id fade = 0;
    island::Activity activity = island::Activity::None;
    std::string signature;
    std::string target;
    std::string pressedTarget;
    std::function<void(float)> progress;
    island::ProgressOutline* ledRing = nullptr;
    // 0 tucked under its neighbour, 1 fully apart.
    float reveal = 0;
    AnimationManager::Id morph = 0;
    bool hovered = false;
  };
  std::array<SplitBubble, 2> splits;
  [[nodiscard]] bool splitHovered() const {
    return std::ranges::any_of(splits, [](const SplitBubble& split) { return split.hovered; });
  }
  // Several jobs in the downloads lane split among themselves: the bubble holds the next job,
  // and clicking it makes that job the capsule's lead.
  bool splitLane = false;
  std::string splitNext;
  std::string splitLead;
  island::PrivacyRotation privacyRotation;
  // Icon last shown in the compact indicator slot, to animate the change to the next one.
  std::string slotIcon;
  bool keyboardMode = false;
  bool captureMenu = false;
  bool captureOptionResize = false;
  capture::LaunchOptions captureOptions;
  std::string captureError;
  bool captureRecording = false;
  int captureRemaining = 0;
  Label* captureCountdownLabel = nullptr;
  island::ActivitySelection activities;
  island::CompactActivity compactActivity;
  std::optional<island::DeviceConnection> connection;
  std::optional<island::NetworkNotice> network;
  Timer activityTimeout;
  std::optional<std::uint32_t> keyboardNotification;
  std::optional<std::uint64_t> keyboardTransferNotice;
  std::optional<std::string> keyboardCard;
  bool heldMedia = false;
  bool suppressHover = false;
  Timer enter;
  Timer leave;
  std::string signature;
  std::optional<std::uint32_t> expandedNotification;
  struct Action {
    float x, y, width, height;
    std::string id;
    std::function<void()> invoke;
  };
  ~Instance() {
    input.setSceneRoot(nullptr);
    TooltipManager::instance().forceDestroy();
    animations.cancelAll();
    if (surface)
      surface->setSceneRoot(nullptr);
  }
  std::vector<Action> actions;
  ProgressBar* seekProgress = nullptr;
  Label* mediaPosition = nullptr;
  Label* recordingLabel = nullptr;
  std::vector<std::pair<std::string, Label*>> captureLabels;
  std::string captureFeedback;
  std::string cameraFeedback;
  Label* awakeLabel = nullptr;
  IslandAudioVisualizer* visualizer = nullptr;
  IslandWidgetHost* hoverWidgets = nullptr;
  struct TimerUi {
    std::string plugin;
    Label* label;
    std::function<void(float)> setFraction;
    // An up-next row's label reads "Starts in 4:07" rather than the bare time.
    bool eventStatus = false;
  };
  std::vector<TimerUi> timerUi;
  struct DownloadUi {
    std::string key;
    Label* percentage;
    ProgressBar* progress;
  };
  std::vector<DownloadUi> downloadUi;
  std::vector<std::pair<std::uint64_t, Label*>> recentTransferUi;
  island::ProgressOutline* downloadLedRing = nullptr;
  bool seeking = false;
  std::int64_t seekLengthUs = 0;
  std::string seekTrackSignature;
  float seekX = 0, seekY = 0, seekWidth = 0, seekFraction = 0;
  std::function<void(float)> seek;
  std::function<void(float)> activeSeek;
  std::string pressedAction;
};

namespace {
  // Cupertino appearance: an always-black Island with white content and Apple's system
  // tints for activities, independent of the shell palette and light/dark mode. Set at
  // the start of each scene build from the instance config.
  bool gCupertino = true;

  constexpr Color kAppleRed = rgba(1.0F, 0.271F, 0.227F);
  constexpr Color kAppleOrange = rgba(1.0F, 0.624F, 0.039F);
  constexpr Color kAppleGreen = rgba(0.188F, 0.82F, 0.345F);
  constexpr Color kAppleBlue = rgba(0.039F, 0.518F, 1.0F);
  constexpr Color kApplePurple = rgba(0.749F, 0.353F, 0.949F);
  // Focus modes, Do Not Disturb among them, are indigo.
  constexpr Color kAppleIndigo = rgba(0.369F, 0.361F, 0.902F);
  // View changes crossfade the capsule's content.
  constexpr float kViewFadeOutMs = Motion::contentMs;
  // The incoming content fades in over half the expand spring's response, by which time the
  // capsule has covered about 95% of its travel.
  constexpr float kViewFadeInMs = Motion::islandExpand.responseMs / 2;
  // Extra space below expanded content; see the layout tail in Island::prepare.
  constexpr float kExpandedBottomInset = 8.0F;

  // The surface height a capsule this tall needs: its 8 px top margin, the capture glow around
  // it, and headroom for the expand spring's overshoot, in 64 px steps so small changes in
  // height don't each resize the surface.
  std::uint32_t surfaceHeightFor(float capsuleHeight, float scale) {
    const float logical = (8.0F + capsuleHeight * 1.1F + 2.0F * island::CaptureGlow::kOutset) * scale;
    return static_cast<std::uint32_t>(std::ceil(logical / 64.0F)) * 64U;
  }
  // The split bubble buds out with a slight overshoot and tucks back without one.
  constexpr float kSplitOutMs = 420.0F;
  constexpr float kSplitInMs = 220.0F;

  // The gap between the capsule and the split bubble scales with the Island's height.
  [[nodiscard]] float splitGap(float height) { return std::round(height * 0.16F); }

  // Up-next events read "Now" once they start; plugin timers keep their clock.
  std::string countdownTime(const island::Countdown& timer) {
    return timer.event && timer.remaining <= 0 ? i18n::tr("island.up-next.now") : timer.time();
  }
  std::string transferMessage(const island::TransferFeedback& feedback) {
    if (!feedback.detail.empty())
      return feedback.detail;
    return i18n::tr(
        feedback.notice == island::TransferNotice::Failed                 ? "island.downloads.failed"
            : feedback.notice == island::TransferNotice::TransferFinished ? "island.downloads.transfer-finished"
                                                                          : "island.downloads.finished"
    );
  }

  std::string recentTransferAge(const island::RecentTransfers::Entry& entry) {
    const auto minutes =
        std::chrono::duration_cast<std::chrono::minutes>(island::RecentTransfers::Clock::now() - entry.received)
            .count();
    if (minutes < 1)
      return i18n::tr("island.downloads.recent-now");
    if (minutes < 60)
      return i18n::tr("island.downloads.recent-minutes", "count", minutes);
    if (minutes < 1440)
      return i18n::tr("island.downloads.recent-hours", "count", minutes / 60);
    return i18n::tr("island.downloads.recent-days", "count", minutes / 1440);
  }

  std::string activityTime(std::chrono::seconds remaining) {
    const auto seconds = remaining.count();
    return seconds >= 3600 ? std::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60)
                           : std::format("{}:{:02}", seconds / 60, seconds % 60);
  }
  std::string eventStatus(const island::Countdown& timer) {
    return timer.remaining > 0 ? i18n::tr("island.up-next.starts-in", "time", timer.time())
                               : i18n::tr("island.up-next.started");
  }
  std::string countdownTitle(const island::Countdown& timer) {
    return timer.event && !timer.title.empty() ? timer.title : i18n::tr(timer.titleKey);
  }
  // Calendar countdowns take the system blue, so they read apart from orange timers.
  Color countdownTint(const island::Countdown& timer) { return timer.event ? kAppleBlue : kAppleOrange; }

  [[nodiscard]] ColorSpec islandFixed(Color color, float alpha) {
    ColorSpec spec = fixedColorSpec(color);
    spec.alpha = alpha;
    return spec;
  }

  // The glass capsule's tint over the compositor's blur, as the shell's glass panels use; opaque
  // when glass is off or the compositor cannot blur behind shell surfaces.
  [[nodiscard]] float glassOpacity(const IslandConfig& cfg) {
    return cfg.glass && ui::material::backgroundBlurAvailable()
        ? ui::material::tintOpacity(ui::material::Kind::Panel, PanelTransparencyMode::Glass)
        : 1.0F;
  }

  [[nodiscard]] ColorSpec islandRole(ColorRole role, float alpha = 1.0F) {
    if (!gCupertino) {
      return colorSpecFromRole(role, alpha);
    }
    const Color white = rgba(1.0F, 1.0F, 1.0F);
    const Color black = rgba(0.0F, 0.0F, 0.0F);
    switch (role) {
    case ColorRole::Surface:
    case ColorRole::Shadow:
      return islandFixed(black, alpha);
    case ColorRole::SurfaceVariant:
      return islandFixed(white, 0.12F * alpha);
    case ColorRole::OnSurfaceVariant:
      return islandFixed(white, 0.62F * alpha);
    case ColorRole::Outline:
      return islandFixed(white, 0.2F * alpha);
    case ColorRole::Hover:
      return islandFixed(white, 0.16F * alpha);
    case ColorRole::OnPrimary:
    case ColorRole::OnSecondary:
    case ColorRole::OnTertiary:
    case ColorRole::OnError:
      return islandFixed(black, alpha);
    case ColorRole::Error:
      return islandFixed(kAppleRed, alpha);
    case ColorRole::Secondary:
      return islandFixed(kAppleGreen, alpha);
    case ColorRole::Tertiary:
      return islandFixed(kApplePurple, alpha);
    case ColorRole::Primary:
    case ColorRole::OnSurface:
    case ColorRole::OnHover:
      break;
    }
    return islandFixed(white, alpha);
  }

  // An activity colour in Cupertino (orange timers, green charge...), else a theme role.
  [[nodiscard]] ColorSpec islandTint(Color apple, ColorRole theme, float alpha = 1.0F) {
    return gCupertino ? islandFixed(apple, alpha) : colorSpecFromRole(theme, alpha);
  }

  [[nodiscard]] Button::ButtonPalette islandButtonPalette(ButtonVariant variant) {
    const auto state = [](float bg, float label, float border = 0.0F) {
      return Button::ButtonStateColors{
          .bg = islandFixed(rgba(1.0F, 1.0F, 1.0F), bg),
          .border = islandFixed(rgba(1.0F, 1.0F, 1.0F), border),
          .label = islandFixed(rgba(1.0F, 1.0F, 1.0F), label),
      };
    };
    switch (variant) {
    case ButtonVariant::Default:
    case ButtonVariant::Secondary:
      // Grey capsules, as in Apple's notification and alert buttons.
      return {
          .normal = state(0.14F, 1.0F),
          .hover = state(0.22F, 1.0F),
          .pressed = state(0.3F, 1.0F),
          .disabled = state(0.08F, 0.35F),
          .selected = std::nullopt
      };
    case ButtonVariant::TabActive:
      return {
          .normal = state(0.2F, 1.0F),
          .hover = state(0.24F, 1.0F),
          .pressed = state(0.3F, 1.0F),
          .disabled = state(0.1F, 0.35F),
          .selected = std::nullopt
      };
    default:
      // Bare symbols: transport controls, tabs and icon buttons show no chrome until hovered.
      return {
          .normal = state(0.0F, variant == ButtonVariant::Tab ? 0.62F : 1.0F),
          .hover = state(0.12F, 1.0F),
          .pressed = state(0.2F, 1.0F),
          .disabled = state(0.0F, 0.3F),
          .selected = std::nullopt
      };
    }
  }

  void setIslandVariant(Button* button, ButtonVariant variant) {
    button->setVariant(variant);
    if (gCupertino) {
      button->setCustomPalette(islandButtonPalette(variant));
    }
  }

  void setIslandStatusStyle(Button* button, ColorSpec tint) {
    auto palette =
        gCupertino ? islandButtonPalette(ButtonVariant::Ghost) : Button::defaultPalette(ButtonVariant::Ghost);
    // Status colours stay visible through pointer interaction, without a background highlight.
    const Button::ButtonStateColors status{.bg = clearColorSpec(), .border = clearColorSpec(), .label = tint};
    palette.normal = palette.hover = palette.pressed = status;
    button->setCustomPalette(std::move(palette));
  }

  void setIslandActionStyle(Button* button) {
    // An outline on keyboard focus and translucent pointer feedback leave the
    // artwork and foreground labels visible throughout the interaction.
    button->setVariant(ButtonVariant::Default);
    auto actionPalette = Button::defaultPalette(ButtonVariant::Ghost);
    actionPalette.hover.bg = islandRole(ColorRole::OnSurface, 0.05F);
    actionPalette.pressed.bg = islandRole(ColorRole::OnSurface, 0.12F);
    button->setCustomPalette(actionPalette);
    button->setZIndex(-1);
  }

  // Reuse the shell's native ring and spinner renderers at the same size.
  class DownloadRing final : public Node {
  public:
    DownloadRing(
        float diameter, float thickness, std::optional<float> progress,
        ColorSpec fillColor = islandRole(ColorRole::Primary), bool charging = false
    )
        : m_fillSpec(fillColor), m_trackSpec(islandRole(ColorRole::OnSurface, 0.16F)), m_charging(charging) {
      setSize(diameter, diameter);
      setHitTestVisible(false);
      if (!progress) {
        auto spinner = std::make_unique<Spinner>();
        spinner->setSpinnerSize(diameter);
        spinner->setThickness(thickness);
        spinner->setColor(m_fillSpec);
        spinner->start();
        addChild(std::move(spinner));
        return;
      }
      auto track = std::make_unique<CountdownRingNode>();
      track->setSize(diameter, diameter);
      track->setThickness(thickness);
      track->setProgress(1);
      m_track = static_cast<CountdownRingNode*>(addChild(std::move(track)));
      auto fill = std::make_unique<CountdownRingNode>();
      fill->setSize(diameter, diameter);
      fill->setThickness(thickness);
      fill->setProgress(*progress);
      fill->setVisible(*progress > 0);
      m_fill = static_cast<CountdownRingNode*>(addChild(std::move(fill)));
      m_palette = paletteChanged().connect([this] { applyPalette(); });
      applyPalette();
    }

    void setAnimationManager(AnimationManager* manager) override {
      Node::setAnimationManager(manager);
      if (!manager) {
        m_pulse = 0;
        return;
      }
      if (m_charging && !m_pulse)
        pulse();
    }
    void setProgress(float progress) {
      if (m_fill) {
        m_fill->setProgress(progress);
        m_fill->setVisible(progress > 0);
      }
    }

  private:
    void pulse() {
      if (!animationManager() || !MotionService::instance().enabled()) {
        if (m_fill)
          m_fill->setOpacity(1);
        return;
      }
      m_pulse = animationManager()->animate(
          0, 1, 1800, Easing::Linear, [this](float t) { m_fill->setOpacity(0.72F + 0.28F * std::cos(t * 6.2831853F)); },
          [this] {
            m_pulse = 0;
            pulse();
          },
          this
      );
    }
    void applyPalette() {
      m_track->setColor(resolveColorSpec(m_trackSpec));
      m_fill->setColor(resolveColorSpec(m_fillSpec));
    }
    ColorSpec m_fillSpec;
    ColorSpec m_trackSpec;
    bool m_charging = false;
    AnimationManager::Id m_pulse = 0;
    CountdownRingNode* m_track = nullptr;
    CountdownRingNode* m_fill = nullptr;
    Signal<>::ScopedConnection m_palette;
  };

  std::string localTime(const char* format) {
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[128]{};
    std::strftime(buffer, sizeof(buffer), format, &local);
    return buffer;
  }
} // namespace

Island::Island() : m_batteryConnections(std::make_unique<island::BatteryConnections>()) {}
Island::~Island() { destroySurfaces(); }

void Island::initializeWidgets(const BarServices& services, IpcService* ipc) {
  m_idle = services.idleInhibitor;
  m_platform = &services.platform;
  m_widgetFactory = std::make_unique<WidgetFactory>(services);
  m_widgetActions.setIpcService(ipc);
  if (services.network && services.network->hasStateSnapshot())
    onNetworkStateChanged(services.network->state());
  onWorkspaceChanged();
}

void Island::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderer, MprisService* mpris,
    NotificationManager* notifications, HttpClient* http, SessionBus* bus, UPowerService* upower,
    BluetoothService* bluetooth, PipeWireService* pipewire, PipeWireSpectrum* spectrum
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderer;
  m_mpris = mpris;
  m_notifications = notifications;
  m_http = http;
  m_upower = upower;
  m_bluetooth = bluetooth;
  m_pipewire = pipewire;
  m_spectrum = spectrum;
  if (bus) {
    try {
      m_downloads = std::make_unique<DownloadProgressService>(*bus);
      m_downloads->changed = [this] { refresh(); };
      m_downloads->reported = [this](const DownloadResult& result, bool announce) {
        reportTransfer(
            result.failed ? island::TransferNotice::Failed : island::TransferNotice::DownloadFinished, result.source,
            result.title, result.detail, announce
        );
      };
    } catch (const std::exception& error) {
      Logger("island").warn("download progress unavailable: {}", error.what());
    }
  }
  onConfigReload();
}

bool Island::enabled() const {
  if (!m_config)
    return false;
  const auto& config = m_config->config();
  return config.island.enabled || std::ranges::any_of(config.bars, [](const auto& bar) {
           return bar.presentation == BarPresentation::Island
               || std::ranges::any_of(bar.monitorOverrides, [](const auto& o) {
                    return o.presentation == BarPresentation::Island;
                  });
         });
}
bool Island::osdVisible() const { return enabled() && m_osd.has_value(); }

std::vector<island::Battery> Island::batteries(const IslandConfig& cfg, wl_output* output) const {
  const auto* info = m_wayland->findOutputByWl(output);
  return island::batterySnapshot(
      m_upower ? m_upower->batteryDevices() : std::vector<UPowerDeviceInfo>{},
      m_bluetooth ? m_bluetooth->devices() : std::vector<BluetoothDeviceInfo>{}, m_config->config().battery,
      m_upower ? m_upower->defaultSystemBattery() : nullptr, m_batteryConnections.get(),
      island::BatteryConnections::Clock::now(), cfg.bluetoothPreviewSeconds, cfg.bluetoothPreviewMonitor,
      info ? info->connectorName : ""
  );
}

std::vector<island::PrivacyActivity> Island::privacy() const {
  return m_privacySummary.snapshot(
      m_pipewire ? m_pipewire->privacyState() : PrivacyState{}, m_config->config().shell.privacy
  );
}

std::optional<std::chrono::seconds> Island::awakeRemaining() const {
  const auto remaining = m_idle && m_idle->enabled() ? m_idle->remaining() : std::nullopt;
  return remaining && remaining->count() > 0 ? remaining : std::nullopt;
}

std::vector<island::Countdown> Island::countdowns() const {
  auto& registry = scripting::PluginRegistry::instance();
  auto& store = scripting::PluginStateStore::instance();
  const auto value = [&store](const std::string& id, const std::string& key) {
    const auto json = store.get(id, key);
    return json ? nlohmann::json::parse(*json, nullptr, false) : nlohmann::json{};
  };
  std::vector<island::Countdown> result;
  if (registry.hasEntry("noctalia/timer:timer"))
    if (auto timer = island::timerSnapshot(
            value("noctalia/timer", "timer.state"), value("noctalia/timer", "timer.remaining"),
            value("noctalia/timer", "timer.duration")
        ))
      result.push_back(*timer);
  if (registry.hasEntry("thepunkoff/pomodoro:pomodoro"))
    if (auto timer = island::pomodoroSnapshot(
            value("thepunkoff/pomodoro", "pomodoro.state"), value("thepunkoff/pomodoro", "pomodoro.sessionData")
        ))
      result.push_back(*timer);
  if (m_calendar != nullptr && m_calendar->enabled() && m_calendar->hasData()) {
    int minutes = 0;
    for (const auto& inst : m_instances)
      minutes = std::max(minutes, inst->config.upNextMinutes);
    if (auto event = island::upNextSnapshot(
            m_calendar->snapshot().events, std::chrono::system_clock::now(), minutes, m_dismissedEvents
        ))
      result.push_back(std::move(*event));
  }
  std::ranges::stable_sort(result, [](const auto& a, const auto& b) {
    if (a.active != b.active)
      return a.active;
    if (a.running != b.running)
      return a.running;
    return a.plugin < b.plugin;
  });
  return result;
}

std::vector<DownloadProgress> Island::progressActivities() const {
  auto result = m_downloads ? m_downloads->active() : std::vector<DownloadProgress>{};
  for (const auto& activity : m_scriptActivities) {
    if (activity.status == island::TransferStatus::Failed)
      continue;
    result.push_back({
        .desktopId = "script:" + activity.id,
        .name = activity.title,
        .progress = activity.progress.value_or(0.0),
        .determinate = activity.progress.has_value(),
        .phase = activity.status == island::TransferStatus::Paused ? "paused" : "working",
        .icon = activity.icon.empty() ? "terminal-2" : activity.icon,
        .key = "script:" + activity.id,
    });
  }
  std::ranges::stable_partition(result, [](const auto& activity) { return !activity.paused(); });
  return result;
}

namespace {
  constexpr auto kScriptActivityLifetime = std::chrono::hours(1);
}

void Island::expireScriptActivities() {
  const auto now = std::chrono::steady_clock::now();
  const auto before = m_scriptActivities.size();
  std::erase_if(m_scriptActivities, [now](const auto& activity) {
    return now - activity.updated >= kScriptActivityLifetime;
  });
  if (m_scriptActivities.empty()) {
    m_scriptActivityExpiry.stop();
  } else {
    const auto oldest = std::ranges::min(m_scriptActivities, {}, &ScriptActivity::updated).updated;
    m_scriptActivityExpiry.start(
        std::chrono::ceil<std::chrono::milliseconds>(oldest + kScriptActivityLifetime - now) + 1ms,
        [this] { expireScriptActivities(); }
    );
  }
  if (m_scriptActivities.size() != before)
    refresh();
}

bool Island::startScriptActivity(const std::string& id, const std::string& title, const std::string& icon) {
  auto it = std::ranges::find(m_scriptActivities, id, &ScriptActivity::id);
  if (it == m_scriptActivities.end())
    it = m_scriptActivities.insert(m_scriptActivities.end(), ScriptActivity{.id = id});
  // Starting again restarts the activity, as an indeterminate one until progress arrives.
  it->title = title;
  it->icon = icon;
  it->progress.reset();
  it->status = island::TransferStatus::Running;
  it->updated = std::chrono::steady_clock::now();
  expireScriptActivities();
  refresh();
  return true;
}

bool Island::updateScriptActivity(
    const std::string& id, std::optional<std::optional<double>> progress, const std::string& title,
    const std::string& icon, std::optional<island::TransferStatus> status
) {
  const auto it = std::ranges::find(m_scriptActivities, id, &ScriptActivity::id);
  if (it == m_scriptActivities.end())
    return false;
  if (progress)
    it->progress = *progress ? std::optional{std::clamp(**progress, 0.0, 1.0)} : std::nullopt;
  if (!title.empty())
    it->title = title;
  if (!icon.empty())
    it->icon = icon;
  const bool failed = status == island::TransferStatus::Failed && it->status != island::TransferStatus::Failed;
  if (status)
    it->status = *status;
  it->updated = std::chrono::steady_clock::now();
  if (failed)
    reportTransfer(island::TransferNotice::Failed, {.name = it->title});
  expireScriptActivities();
  refresh();
  return true;
}

bool Island::endScriptActivity(const std::string& id) {
  const auto found = std::ranges::find(m_scriptActivities, id, &ScriptActivity::id);
  if (found == m_scriptActivities.end())
    return false;
  const bool finished = found->status == island::TransferStatus::Running && found->progress && *found->progress >= 1.0;
  const auto title = found->title;
  m_scriptActivities.erase(found);
  if (finished)
    reportTransfer(island::TransferNotice::TransferFinished, {.name = title});
  expireScriptActivities();
  refresh();
  return true;
}

void Island::reportTransfer(
    island::TransferNotice notice, DownloadSource source, std::string title, std::string detail, bool announce
) {
  if (!enabled())
    return;
  auto feedback = island::TransferFeedback{
      notice, std::move(source), ++m_transferNoticeSerial, std::move(title), std::move(detail)
  };
  m_recentTransfers.remember(feedback);
  if (!announce) {
    refresh();
    return;
  }
  m_transferNotice = std::move(feedback);
  m_transferNoticeTimeout.start(5s, [this] {
    m_transferNotice.reset();
    refresh();
  });
  refresh();
}

bool Island::transferApp(const DownloadSource& source, bool activate) const {
  if (!m_platform || source.desktopId.empty())
    return false;
  const auto windows =
      m_platform->windowsForApp(StringUtils::toLower(source.desktopId), StringUtils::toLower(source.wmClass));
  if (windows.empty())
    return false;
  if (activate)
    m_platform->activateToplevelInfo(windows.front());
  return true;
}

bool Island::activateTransferSource(Instance& inst, const DownloadSource& source) {
  if (!transferApp(source))
    return false;
  m_transferActivation.stop();
  // Release the layer's exclusive keyboard grab before giving focus back.
  bool releasedKeyboard = false;
  for (auto& other : m_instances)
    if (other->keyboardMode) {
      releasedKeyboard = true;
      releaseKeyboard(*other);
    }
  if (releasedKeyboard) {
    // A commit acknowledgement can precede the compositor applying the grab
    // change. Wait for keyboard-leave without blocking input or animation.
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    m_transferActivation.startRepeating(16ms, [this, source, deadline] {
      if (std::ranges::any_of(m_instances, [](const auto& other) {
            return other->keyboardMode || other->panelHosted;
          })) {
        m_transferActivation.stop();
        return;
      }
      const auto focused = m_wayland->lastKeyboardSurface();
      if (focused && std::ranges::any_of(m_instances, [focused](const auto& other) {
            return other->surface->wlSurface() == focused;
          })) {
        if (std::chrono::steady_clock::now() >= deadline)
          m_transferActivation.stop();
        return;
      }
      m_transferActivation.stop();
      (void)transferApp(source, true);
    });
  } else if (!transferApp(source, true))
    return false;
  inst.hovered = false;
  inst.suppressHover = inst.inside;
  inst.enter.stop();
  TooltipManager::instance().forceDestroy();
  return true;
}

void Island::activateTransfer(Instance& inst, const std::string& key) {
  // A stale gesture must never open a different row after a job disappears.
  const auto downloads = progressActivities();
  const auto found = std::ranges::find(downloads, key, &DownloadProgress::key);
  if (found != downloads.end() && activateTransferSource(inst, found->source))
    refresh();
}

void Island::activateTransferNotice(Instance& inst, std::uint64_t serial) {
  if (!m_transferNotice || m_transferNotice->serial != serial)
    return;
  const auto source = m_transferNotice->source;
  if (!activateTransferSource(inst, source))
    return;
  m_transferNotice.reset();
  m_transferNoticeTimeout.stop();
  refresh();
}

void Island::activateRecentTransfer(Instance& inst, std::uint64_t serial) {
  const auto* entry = m_recentTransfers.find(serial);
  // A queued gesture belongs to this result, even if history has since rotated.
  if (entry && activateTransferSource(inst, entry->feedback.source))
    refresh();
}

void Island::timerCommand(const island::Countdown& timer, const std::string& command) {
  if (!scripting::PluginRegistry::instance().hasEntry(timer.panel))
    return;
  // A queued resume must not turn an ended or reset timer into a new timer.
  const auto current = countdowns();
  const auto live = std::ranges::find(current, timer.plugin, &island::Countdown::plugin);
  if (live == current.end() || !island::acceptsCountdownCommand(timer, *live, command))
    return;
  scripting::PluginStateStore::instance().set(timer.plugin, timer.commandKey(), nlohmann::json(command).dump());
  refresh();
}

void Island::destroySurfaces() {
  m_transferActivation.stop();
  if (closeHostedPanel)
    closeHostedPanel();
  m_flowTimer.stop();
  for (auto& inst : m_instances) {
    releaseFlow(*inst);
    inst->animations.cancelAll();
    inst->surface->setSceneRoot(nullptr);
  }
  m_instances.clear();
}

void Island::onConfigReload() {
  destroySurfaces();
  m_tick.stop();
  m_osd.reset();
  m_osdTimeout.stop();
  m_networkTimeout.stop();
  if (enabled()) {
    onOutputChange();
    m_tick.startRepeating(1s, [this] { refresh(); });
  } else {
    m_transferNotice.reset();
    m_transferNoticeTimeout.stop();
    updateNotificationPreview();
  }
}

void Island::onOutputChange() {
  if (!enabled() || !m_wayland || !m_renderContext)
    return;
  const auto& config = m_config->config();
  auto bars = config.bars;
  const bool managed = std::ranges::any_of(bars, [](const auto& bar) {
    return bar.presentation == BarPresentation::Island || std::ranges::any_of(bar.monitorOverrides, [](const auto& o) {
             return o.presentation == BarPresentation::Island;
           });
  });
  if (!managed && config.island.enabled) {
    BarConfig legacy;
    legacy.name = "__legacy_island";
    legacy.presentation = BarPresentation::Island;
    legacy.island = config.island;
    legacy.scale = config.island.scale;
    legacy.reserveSpace = config.island.reserveSpace;
    bars.push_back(std::move(legacy));
  }
  const auto selected = [&](const BarConfig& bar, const WaylandOutput& output) {
    if (bar.name == "__legacy_island"
        && !config.island.monitors.empty()
        && std::ranges::none_of(config.island.monitors, [&](const auto& selector) {
             return outputMatchesSelector(selector, output);
           }))
      return false;
    const auto resolved = ConfigService::resolveForOutput(bar, output);
    return resolved.enabled && resolved.presentation == BarPresentation::Island;
  };
  // Match Orbit's explicit monitor choice; never mirror private alerts to another output as a fallback.
  // End a borrow before resizing/removing an output's surface.
  if (closeHostedPanel && std::ranges::any_of(m_instances, [](const auto& inst) { return inst->panelHosted; }))
    closeHostedPanel();
  std::erase_if(m_instances, [&](const auto& inst) {
    const auto* output = m_wayland->findOutputByWl(inst->output);
    const auto bar = std::ranges::find(bars, inst->barConfig.name, &BarConfig::name);
    const bool remove =
        !output || !output->done || !output->hasUsableGeometry() || bar == bars.end() || !selected(*bar, *output);
    if (remove)
      releaseFlow(*inst);
    return remove;
  });
  for (const auto& bar : bars)
    for (const auto& output : m_wayland->outputs()) {
      if (!output.done || !output.output || !output.hasUsableGeometry() || !selected(bar, output))
        continue;
      const auto resolved = ConfigService::resolveForOutput(bar, output);
      auto cfg = resolved.island;
      cfg.scale = resolved.scale;
      cfg.reserveSpace = resolved.reserveSpace;
      const float scale = std::min(
          {cfg.scale * m_config->config().accessibility.uiScale,
           static_cast<float>(output.effectiveLogicalWidth()) / 480.0F,
           static_cast<float>(output.effectiveLogicalHeight()) / 300.0F}
      );
      // Keep the compositor viewport stable across capsule/panel transitions.
      // Recentring a resized layer surface adds a second, sideways animation on
      // Hyprland. Only the capsule's scene geometry and input region should move.
      const auto sw = static_cast<std::uint32_t>(output.effectiveLogicalWidth());
      const auto sh = static_cast<std::uint32_t>(output.effectiveLogicalHeight());
      const auto initialHeight = std::min(sh, surfaceHeightFor(cfg.height, scale));
      const auto existing = std::ranges::find_if(m_instances, [&](const auto& inst) {
        return inst->output == output.output && inst->barConfig.name == bar.name;
      });
      if (existing != m_instances.end()) {
        auto& current = **existing;
        current.barConfig = resolved;
        current.config = cfg;
        current.scale = scale;
        current.signature.clear();
        current.surfaceWidth = sw;
        current.outputHeight = static_cast<float>(sh);
        current.surfaceHeight = current.panelHosted ? sh : std::min(sh, std::max(current.surfaceHeight, initialHeight));
        current.surface->requestSize(sw, current.surfaceHeight);
        current.surface->setExclusiveZone(
            cfg.reserveSpace ? static_cast<int>(std::ceil((cfg.height + 12) * scale)) : -1
        );
        current.surface->requestUpdate();
        continue;
      }
      auto inst = std::make_unique<Instance>();
      inst->barConfig = resolved;
      inst->config = cfg;
      inst->output = output.output;
      inst->scale = scale;
      inst->surfaceWidth = sw;
      inst->surfaceHeight = initialHeight;
      inst->outputHeight = static_cast<float>(sh);
      LayerSurfaceConfig surfaceConfig{
          .nameSpace = "noctalia-island",
          .layer = resolved.layer == "overlay" ? LayerShellLayer::Overlay : LayerShellLayer::Top,
          .anchor = LayerShellAnchor::Top,
          .width = sw,
          .height = initialHeight,
          .exclusiveZone = cfg.reserveSpace ? static_cast<int>(std::ceil((cfg.height + 12) * inst->scale)) : -1,
          .keyboard = LayerShellKeyboard::None,
          .defaultWidth = sw,
          .defaultHeight = initialHeight,
      };
      inst->surface = std::make_unique<LayerSurface>(*m_wayland, surfaceConfig);
      inst->surface->setRenderContext(m_renderContext);
      auto* ptr = inst.get();
      inst->input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_wayland->setCursorShape(serial, shape);
      });
      inst->input.setHoverChangeCallback([ptr](InputArea*, InputArea* next) {
        if (next)
          next->setTooltipPlacement(TooltipPlacement::Below);
        TooltipManager::instance().onHoverChange(next, ptr->surface->layerSurface(), ptr->output);
      });
      inst->input.setFocusChangeCallback([ptr](InputArea* old, InputArea* next) {
        if (old)
          if (auto* button = dynamic_cast<Button*>(old->parent()))
            button->setKeyboardFocusHint(false);
        if (next)
          if (auto* button = dynamic_cast<Button*>(next->parent()))
            button->setKeyboardFocusHint(ptr->keyboardMode);
        if (next && ptr->keyboardMode && ptr->activityScroll) {
          auto* scroll = ptr->activityScroll;
          float top = 0;
          Node* node = next;
          for (; node && node != scroll->content(); node = node->parent())
            top += node->y();
          if (node) {
            const float bottom = top + next->height();
            const float offset = scroll->scrollOffset();
            if (top < offset)
              scroll->setScrollOffset(top);
            else if (bottom > offset + scroll->contentViewportHeight())
              scroll->setScrollOffset(bottom - scroll->contentViewportHeight());
          }
        }
      });
      inst->surface->setConfigureCallback([ptr](std::uint32_t, std::uint32_t) {
        ptr->signature.clear();
        ptr->surface->requestUpdate();
      });
      inst->surface->setPrepareFrameCallback([this, ptr](bool, bool) { prepare(*ptr); });
      inst->surface->setAnimationManager(&inst->animations);
      inst->surface->setFrameTickCallback([ptr](float dt) {
        if (!ptr->panelHosted && ptr->visualizer)
          ptr->visualizer->onFrameTick(dt);
        if (!ptr->panelHosted && ptr->hoverWidgets)
          ptr->hoverWidgets->tickWidgets(dt);
      });
      if (!inst->surface->initialize(output.output))
        continue;
      inst->surface->setInputRegion({});
      m_instances.push_back(std::move(inst));
    }
  if (m_notification
      && m_notification->urgency != Urgency::Critical
      && m_notification->timeout > 0
      && std::ranges::none_of(m_instances, [](const auto& inst) { return inst->inside || inst->keyboardMode; }))
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  if (m_platform)
    onWorkspaceChanged();
  else
    refresh();
}

bool Island::trackPreview(const IslandConfig& cfg, wl_output* output) const {
  const auto* info = m_wayland->findOutputByWl(output);
  return info
      && m_mediaActivity.announcing(island::MediaActivity::Clock::now(), cfg.trackPreviewSeconds)
      && m_mediaActivity.targets(cfg.trackPreviewMonitor, info->connectorName);
}

void Island::onNetworkStateChanged(const NetworkState& state) {
  const auto* output = m_wayland
      ? m_wayland->findOutputByWl(
            m_platform ? m_platform->preferredInteractiveOutput() : m_wayland->lastPointerOutput()
        )
      : nullptr;
  m_networkActivity.update(state, island::NetworkActivity::Clock::now(), output ? output->connectorName : "");
  refresh();
}

void Island::refresh() {
  if (!enabled())
    return;
  const auto privacyList = privacy();
  const auto screen = std::ranges::find(privacyList, PrivacyCaptureKind::Screen, &island::PrivacyActivity::kind);
  const bool desktopShared = screen != privacyList.end();
  m_screenSessions.update(
      desktopShared ? screen->apps : std::vector<std::string>{}, island::CaptureSessions::Clock::now()
  );
  const auto camera = std::ranges::find(privacyList, PrivacyCaptureKind::Camera, &island::PrivacyActivity::kind);
  m_cameraSessions.update(
      camera != privacyList.end() ? camera->apps : std::vector<std::string>{}, island::CaptureSessions::Clock::now()
  );
  const bool sharingChanged = desktopShared != m_desktopShared;
  m_desktopShared = desktopShared;
  const auto now = island::BatteryConnections::Clock::now();
  std::vector<std::string> outputs;
  for (const auto& inst : m_instances)
    if (const auto* output = m_wayland->findOutputByWl(inst->output))
      if (std::ranges::find(outputs, output->connectorName) == outputs.end())
        outputs.push_back(output->connectorName);
  const auto* focused =
      m_wayland->findOutputByWl(m_platform ? m_platform->preferredInteractiveOutput() : m_wayland->lastPointerOutput());
  island::PreviewTarget target;
  target.reconcile(outputs, focused ? focused->connectorName : "");
  m_batteryConnections->update(
      m_bluetooth ? m_bluetooth->devices() : std::vector<BluetoothDeviceInfo>{}, now, target.output
  );
  m_batteryConnections->updatePower(
      m_upower ? m_upower->batteryDevices() : std::vector<UPowerDeviceInfo>{}, now, target.output,
      m_bluetooth ? m_bluetooth->devices() : std::vector<BluetoothDeviceInfo>{}
  );
  m_batteryConnections->reconcileOutputs(outputs, target.output);
  m_networkActivity.advance(now);
  m_networkActivity.reconcileOutputs(outputs, target.output);
  hideDndSuppressed();
  updateNotificationPreview();
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const std::string track = player ? player->busName + logicalTrackSignature(*player) : "";
  m_mediaActivity.update(
      player ? island::mediaAnnouncementKey(*player) : "", player ? player->playbackStatus : "", now, target.output
  );
  m_mediaActivity.reconcileOutputs(outputs, target.output);
  m_trackSignature = track;
  std::optional<TimePoint> mediaExpiry;
  auto batteryExpiry = m_batteryConnections->nextExpiry(now, island::BatteryConnections::kGlowSeconds);
  auto networkExpiry = m_networkActivity.nextChange();
  for (const auto& inst : m_instances) {
    const auto& cfg = inst->config;
    if (const auto expiry = m_mediaActivity.nextExpiry(now, cfg.trackPreviewSeconds, cfg.pausedMediaSeconds);
        expiry && (!mediaExpiry || *expiry < *mediaExpiry))
      mediaExpiry = expiry;
    if (const auto expiry = m_batteryConnections->nextExpiry(now, cfg.bluetoothPreviewSeconds);
        expiry && (!batteryExpiry || *expiry < *batteryExpiry))
      batteryExpiry = expiry;
    if (const auto expiry = m_networkActivity.nextExpiry(now, cfg.networkPreviewSeconds);
        expiry && (!networkExpiry || *expiry < *networkExpiry))
      networkExpiry = expiry;
  }
  if (mediaExpiry)
    m_mediaTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*mediaExpiry - now), [this] { refresh(); });
  else
    m_mediaTimeout.stop();
  if (batteryExpiry)
    m_batteryTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*batteryExpiry - now), [this] { refresh(); });
  else
    m_batteryTimeout.stop();
  if (networkExpiry)
    m_networkTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*networkExpiry - now), [this] { refresh(); });
  else
    m_networkTimeout.stop();
  const auto timers = countdowns();
  const bool timerActive = std::ranges::any_of(timers, [](const auto& timer) { return timer.active; });
  const bool downloadActive = !progressActivities().empty();
  for (auto& inst : m_instances) {
    const auto& cfg = inst->config;
    const auto* output = m_wayland->findOutputByWl(inst->output);
    inst->connection = island::deviceConnection(
        *m_batteryConnections, m_bluetooth ? m_bluetooth->devices() : std::vector<BluetoothDeviceInfo>{},
        m_upower ? m_upower->batteryDevices() : std::vector<UPowerDeviceInfo>{},
        m_pipewire ? m_pipewire->defaultSink() : nullptr, now, cfg.bluetoothPreviewSeconds, cfg.bluetoothPreviewMonitor,
        output ? output->connectorName : ""
    );
    inst->network = m_networkActivity.preview(
        now, cfg.networkPreviewSeconds, cfg.networkPreviewMonitor, output ? output->connectorName : ""
    );
    updateVisibility(*inst);
    inst->compactActivity.update(
        {player && m_mediaActivity.compact(now, cfg.pausedMediaSeconds), downloadActive, timerActive, false,
         awakeRemaining().has_value()},
        // Split activities are all in view, so they never cycle.
        cfg.activityPriority, cfg.cycleActivities && !cfg.splitActivities, cfg.activityCycleSeconds,
        inst->inside
            || inst->hovered
            || inst->keyboardMode
            || inst->panelHosted
            || !inst->wantsVisible
            || m_notification
            || m_osd
            || m_transferNotice.has_value()
            || inst->connection.has_value()
            || inst->network.has_value()
            || ScreenRecorder::instance().active(),
        now
    );
    if (const auto expiry = inst->compactActivity.nextExpiry())
      inst->activityTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*expiry - now), [this] { refresh(); });
    else
      inst->activityTimeout.stop();
    if (!inst->panelHosted || sharingChanged) {
      inst->surface->requestUpdate();
    }
  }
}

void Island::onWorkspaceChanged() {
  if (!m_platform)
    return;
  for (auto& inst : m_instances) {
    std::string active;
    for (const auto& workspace : m_platform->workspaces(inst->output))
      if (workspace.active) {
        active = workspace.id;
        break;
      }
    if (!inst->workspaceId.empty()
        && active != inst->workspaceId
        && inst->barConfig.isAutoHideEnabled()
        && inst->barConfig.showOnWorkspaceSwitch) {
      inst->workspacePeek = true;
      inst->peek.start(std::chrono::milliseconds(450), [this, ptr = inst.get()] {
        ptr->workspacePeek = false;
        refresh();
      });
    }
    inst->workspaceId = active;
  }
  refresh();
}

void Island::updateVisibility(Instance& inst) {
  const bool smartVisible = inst.barConfig.smartAutoHide
      && m_platform
      && noctalia::bar::smartAutoHideWantsPinnedVisible(*m_platform, inst.output);
  const bool visible = !inst.barConfig.isAutoHideEnabled()
      || smartVisible
      || inst.inside
      || inst.hovered
      || inst.keyboardMode
      || inst.panelHosted
      || inst.workspacePeek
      || (inst.config.revealOnTrackChange && trackPreview(inst.config, inst.output))
      || m_osd
      || m_notification
      || m_transferNotice.has_value()
      || inst.connection.has_value()
      || inst.network.has_value()
      || m_desktopShared
      || ScreenRecorder::instance().active();
  if (inst.wantsVisible == visible)
    return;
  inst.wantsVisible = visible;
  inst.animations.cancel(inst.hideAnimation);
  inst.hideAnimation = inst.animations.animate(
      inst.visibility, visible ? 1.0F : 0.0F, visible ? Motion::revealMs : Motion::dismissMs,
      visible ? Motion::reveal : Motion::dismiss, [this, &inst](float value) {
        inst.visibility = value;
        geometry(inst);
      }
  );
  inst.surface->requestFrameTick();
}

bool Island::showOsd(const OsdContent& content) {
  if (!enabled() || m_instances.empty())
    return false;
  // Track announcements belong in the clock slot, with no separate large OSD.
  if (content.kind == OsdKind::Media) {
    refresh();
    return true;
  }
  // The timed activity is its own feedback, including extension and ending it.
  if (content.kind == OsdKind::Caffeine && (awakeRemaining() || std::ranges::any_of(m_instances, [](const auto& inst) {
                                              return inst->previousView == island::View::Awake
                                                  || inst->previousView == island::View::AwakeActivity;
                                            }))) {
    refresh();
    return true;
  }
  // The input card already shows mute changes, including microphone hardware keys.
  if (content.kind == OsdKind::Microphone && std::ranges::any_of(m_instances, [](const auto& inst) {
        return !inst->panelHosted
            && inst->previousView == island::View::Microphone
            && (inst->hovered || inst->keyboardMode);
      })) {
    refresh();
    return true;
  }
  // A panel open in the Island already shows what changed (its sliders and toggles caused it),
  // and the OSD cannot show until the panel closes; queuing it would replay a stale volume or
  // toggle OSD the moment it does. Report it handled so no standalone OSD appears either.
  if (std::ranges::any_of(m_instances, [](const auto& inst) { return inst->panelHosted; })
      || std::chrono::steady_clock::now() < m_osdQuietUntil)
    return true;
  m_osd = content;
  m_osdTimeout.start(1800ms, [this] {
    m_osd.reset();
    refresh();
  });
  refresh();
  return true;
}

bool Island::onNotification(const Notification& n, NotificationEvent event) {
  for (auto& inst : m_instances)
    if (inst->keyboardMode
        && !inst->captureMenu
        && inst->captureRemaining <= 0
        && ((event == NotificationEvent::Closed && inst->keyboardNotification == n.id)
            || (event == NotificationEvent::Added
                && inst->keyboardNotification != n.id
                && !(m_notifications->dndSuppresses(n)))))
      releaseKeyboard(*inst);
  if (event == NotificationEvent::Closed) {
    std::erase_if(m_urgentNotifications, [&](const auto& queued) { return queued.id == n.id; });
    if (m_notification && m_notification->id == n.id) {
      m_notification.reset();
      refresh();
    }
    return false;
  }
  if (!enabled() || m_instances.empty())
    return false;
  // Preserve Noctalia's full reply editor for notifications requiring text input.
  for (std::size_t i = 0; i + 1 < n.actions.size(); i += 2)
    if (n.actions[i] == "inline-reply")
      return false;
  if (m_notifications->dndSuppresses(n))
    return true;
  if (auto queued = std::ranges::find(m_urgentNotifications, n.id, &Notification::id);
      queued != m_urgentNotifications.end()) {
    if (n.urgency == Urgency::Critical) {
      *queued = n;
      m_notifications->pauseExpiry(n.id);
    } else {
      m_urgentNotifications.erase(queued);
    }
    return true;
  }
  if (m_notification && m_notification->id != n.id && m_notification->urgency == Urgency::Critical) {
    if (n.urgency == Urgency::Critical) {
      m_urgentNotifications.push_back(n);
      m_notifications->pauseExpiry(n.id);
    }
    refresh();
    return true;
  }
  if (event == NotificationEvent::Updated
      && (!m_notification || m_notification->id != n.id)
      && n.urgency != Urgency::Critical)
    return true;
  if (m_notification && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  const bool newPreview = !m_notification || m_notification->id != n.id || m_notification->urgency != n.urgency;
  if (newPreview)
    for (auto& inst : m_instances)
      inst->expandedNotification.reset();
  if (m_notification && m_notification->id == n.id && m_notification->imageData != n.imageData)
    for (auto& inst : m_instances)
      inst->signature.clear();
  m_notification = n;
  if (n.urgency == Urgency::Critical) {
    m_notificationDeadline.reset();
    m_notificationPreviewTimer.stop();
  } else if (newPreview) {
    m_notificationDeadline = Clock::now() + 5s;
    m_notificationPreviewTimer.start(5s, [this] { refresh(); });
  }
  if (n.urgency == Urgency::Critical || (n.timeout > 0 && std::ranges::any_of(m_instances, [](const auto& inst) {
                                           return inst->inside || inst->keyboardMode;
                                         })))
    m_notifications->pauseExpiry(n.id);
  refresh();
  return true;
}

void Island::hideDndSuppressed() {
  std::erase_if(m_urgentNotifications, [&](const auto& queued) {
    if (m_notifications->dndSuppresses(queued)) {
      if (queued.timeout > 0)
        m_notifications->resumeExpiry(queued.id, queued.timeout);
      return true;
    }
    return false;
  });
  if (m_notification && m_notifications && m_notifications->dndSuppresses(*m_notification)) {
    if (m_notification->timeout > 0)
      m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
    m_notification.reset();
    for (auto& inst : m_instances)
      inst->surface->requestUpdate();
  }
}

void Island::updateNotificationPreview() {
  if (m_instances.empty() || !enabled()) {
    if (m_notification && m_notification->timeout > 0)
      m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
    for (const auto& queued : m_urgentNotifications)
      if (queued.timeout > 0)
        m_notifications->resumeExpiry(queued.id, queued.timeout);
    m_urgentNotifications.clear();
    m_notification.reset();
    m_notificationDeadline.reset();
    m_notificationPreviewTimer.stop();
    return;
  }
  if (!m_notification && !m_urgentNotifications.empty()) {
    m_notification = m_urgentNotifications.front();
    m_urgentNotifications.erase(m_urgentNotifications.begin());
    m_notificationDeadline.reset();
    m_notificationPreviewTimer.stop();
  }
  if (m_notification
      && m_notification->urgency != Urgency::Critical
      && m_notificationDeadline
      && Clock::now() >= *m_notificationDeadline
      && std::ranges::none_of(m_instances, [](const auto& inst) { return inst->inside || inst->keyboardMode; })) {
    const auto id = m_notification->id;
    m_notification.reset();
    m_notificationDeadline.reset();
    m_notificationPreviewTimer.stop();
    // Notification updates can call refresh from inside the manager's event dispatch.
    // Close after that dispatch finishes so its current notification remains valid.
    DeferredCall::callLater([this, id, alive = std::weak_ptr<void>(m_lifetime)] {
      if (!alive.expired())
        m_notifications->close(id, CloseReason::Expired);
    });
  }
}

void Island::dismissNotification() {
  if (!m_notification)
    return;
  const auto id = m_notification->id;
  m_notification.reset();
  m_notifications->close(id, CloseReason::Dismissed);
  refresh();
}

void Island::hideNotificationPreviews() {
  for (auto& inst : m_instances)
    if (inst->keyboardNotification)
      releaseKeyboard(*inst);
  if (m_notification && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  for (const auto& queued : m_urgentNotifications)
    if (queued.timeout > 0)
      m_notifications->resumeExpiry(queued.id, queued.timeout);
  m_notification.reset();
  m_urgentNotifications.clear();
  m_notificationDeadline.reset();
  m_notificationPreviewTimer.stop();
  refresh();
}

void Island::geometry(Instance& inst) {
  if (!inst.root)
    return;
  // A fully hidden capsule needs neither waveform samples nor artwork frames.
  // Keep both running during the reveal/dismiss motion, and resume on reveal
  // even when the retained media card does not need rebuilding.
  if (inst.visualizer)
    inst.visualizer->setActive(!inst.panelHosted && inst.visibility > 0.001F);
  syncFlowTimer();
  const float s = inst.scale;
  // The capsule stays centred, where panels open from and collapse back to; the split bubble
  // hangs off its right end.
  const float bubble = inst.config.height;
  const float spacing = splitGap(bubble);
  const float splitWidth =
      (std::max(0.0F, inst.splits[0].reveal) + std::max(0.0F, inst.splits[1].reveal)) * (spacing + bubble);
  const float x = (static_cast<float>(inst.surface->width()) - inst.width * s) / 2;
  const float y = (8 - (inst.height + 12) * (1 - inst.visibility)) * s;
  // Each bubble comes out from under its left neighbour (the capsule, then the first bubble),
  // with a little growth as it separates.
  float offset = 0;
  for (auto& split : inst.splits) {
    if (!split.bubble)
      continue;
    const float reveal = split.reveal;
    offset += (spacing + bubble) * reveal;
    const float diameter = bubble * (0.72F + 0.28F * std::min(1.0F, std::max(0.0F, reveal)));
    const float centre = inst.width - bubble / 2 + offset;
    split.bubble->setVisible(reveal > 0.001F);
    split.bubble->setPosition(x + (centre - diameter / 2) * s, y + (bubble - diameter) * s / 2);
    split.bubble->setSize(diameter * s, diameter * s);
    split.bubble->setRadius(diameter * s / 2);
    if (split.area) {
      split.area->setSize(diameter * s, diameter * s);
      split.area->setHitTestVisible(reveal > 0.5F && split.activity != island::Activity::None);
    }
    if (split.content) {
      // The content is laid out for the full bubble; keep it centred while the bubble grows.
      split.content->setPosition((diameter - bubble) * s / 2, (diameter - bubble) * s / 2);
      split.content->setOpacity(std::clamp((reveal - 0.4F) / 0.6F, 0.0F, 1.0F) * split.contentFade);
    }
    if (split.outgoing) {
      split.outgoing->setPosition((diameter - bubble) * s / 2, (diameter - bubble) * s / 2);
      split.outgoing->setOpacity(std::clamp((reveal - 0.4F) / 0.6F, 0.0F, 1.0F) * (1 - split.contentFade));
    }
  }
  inst.background->setPosition(x, y);
  inst.background->setSize(inst.width * s, inst.height * s);
  const float radius = island::surfaceRadius(inst.height * s, s, gCupertino);
  inst.background->setRadius(radius);
  if (inst.progressOutline) {
    inst.progressOutline->setPosition(x, y);
    inst.progressOutline->setGeometry(inst.width * s, inst.height * s, radius, s);
  }
  if (inst.captureGlow)
    inst.captureGlow->setGeometry(x, y, inst.width * s, inst.height * s, radius, s);
  if (inst.flowImage) {
    inst.flowImage->setPosition(0, 0);
    inst.flowImage->setSize(inst.width * s, inst.height * s);
    inst.flowImage->setRadius(radius);
  }
  if (inst.outgoing)
    inst.outgoing->setPosition((inst.width - inst.outgoingWidth) * s / 2, 0);
  if (inst.content) {
    inst.content->setPosition((inst.width - inst.targetWidth) * s / 2, 0);
    // Conceal content until the expanding capsule has room to contain it: the capsule clips its
    // content square, so content larger than the capsule shows cut off with flat edges. A capsule
    // still larger than its content (collapsing) only needs to be close, so the compact view
    // doesn't float in an oversized capsule.
    const float shortfall = std::max(inst.targetWidth - inst.width, inst.targetHeight - inst.height);
    const float excess = std::max(inst.width - inst.targetWidth, inst.height - inst.targetHeight);
    const float room = inst.captureOptionResize
        ? 1.0F
        : std::clamp(1 - shortfall / 10, 0.0F, 1.0F) * std::clamp(1 - excess / 55, 0.0F, 1.0F);
    inst.content->setOpacity(room * inst.contentFade * (1 - inst.outgoingFade));
    inst.content->setHitTestVisible(inst.content->opacity() > 0.1F);
  }
  const std::array<int, 4> inputRegion{
      static_cast<int>(std::floor(x)), 0, static_cast<int>(std::ceil((inst.width + splitWidth) * s)),
      inst.wantsVisible ? static_cast<int>(std::ceil((inst.height + 8) * s)) : 3
  };
  if (inst.inputRegion != inputRegion) {
    inst.inputRegion = inputRegion;
    inst.surface->setInputRegion({InputRect{inputRegion[0], inputRegion[1], inputRegion[2], inputRegion[3]}});
    // A changed input region needs a commit even if no scene node changed.
    inst.surface->requestRedraw();
  }
  fitSurface(inst);
  updateGlass(inst, x, y, radius);
  // Node setters invalidate actual geometry/content changes. Repeated clock or
  // window-title updates must not redraw an otherwise unchanged capsule.
  inst.input.syncPointerHover();
  TooltipManager::instance().syncAnchor(inst.input.hoveredArea());
}

bool Island::connectionAudioOsd(const Instance& inst) const {
  const auto* sink = m_pipewire ? m_pipewire->defaultSink() : nullptr;
  return inst.connection
      && inst.connection->audioOutput
      && !inst.hovered
      && sink
      && m_osd
      && m_osd->kind == OsdKind::Volume
      && !m_osd->showProgress
      && m_osd->value == audioDeviceLabel(*sink);
}

std::optional<std::string> Island::cardActionKey(const Instance& inst) const {
  if (!openPanel)
    return std::nullopt;
  if (inst.previousView == island::View::Connection && inst.connection && !inst.connection->panel.empty())
    return inst.connection->actionKey();
  if (inst.previousView == island::View::Network && inst.network)
    return inst.network->actionKey();
  return std::nullopt;
}

void Island::prepare(Instance& inst) {
  if (!enabled() || inst.panelHosted)
    return;
  UiPhaseScope phase(UiPhase::Layout);
  // Evaluate the visible state before binding a GPU surface. On multiple
  // monitors, even unchanged title notifications otherwise switch EGL targets.
  auto& renderer = inst.surface->renderTarget().renderer();
  const auto& cfg = inst.config;
  gCupertino = cfg.appearance == IslandAppearance::Cupertino;
  const ColorSpec foreground = islandRole(ColorRole::OnSurface);
  const ColorSpec muted = islandRole(ColorRole::OnSurfaceVariant);
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const bool playing = player && player->playbackStatus == "Playing";
  const bool preview = player && trackPreview(cfg, inst.output);
  const std::string trackArtist = player ? joinedArtists(player->artists) : "";
  const std::string announcement = preview ? (player->title.empty() ? trackArtist : player->title) : "";
  const std::string announcementArtist = preview && !player->title.empty() ? trackArtist : "";
  auto downloads = progressActivities();
  const auto& recentTransfers = m_recentTransfers.entries();
  const bool downloadCard = !downloads.empty() || !recentTransfers.empty();
  // A bubble click put this job in the capsule; it stays there while it runs.
  island::orderTransfers(downloads, inst.splitLead);
  const auto timers = countdowns();
  const bool timerActive = !timers.empty() && timers.front().active;
  const auto awake = awakeRemaining();
  const bool recording = ScreenRecorder::instance().active();
  const bool captureActive = recording || !m_screenSessions.sessions().empty();
  const bool cameraActive = !m_cameraSessions.sessions().empty();
  if (!m_screenSessions.sessions().contains(inst.captureFeedback))
    inst.captureFeedback.clear();
  if (!m_cameraSessions.sessions().contains(inst.cameraFeedback))
    inst.cameraFeedback.clear();
  auto privacyList = privacy();
  const auto mic = std::ranges::find(privacyList, PrivacyCaptureKind::Microphone, &island::PrivacyActivity::kind);
  const auto microphone = mic != privacyList.end() ? std::optional{*mic} : std::nullopt;
  std::vector<AudioNode> microphoneInputs;
  if (microphone && m_pipewire)
    for (const auto id : microphone->sourceIds)
      if (const auto input = std::ranges::find(m_pipewire->state().sources, id, &AudioNode::id);
          input != m_pipewire->state().sources.end() && input->available)
        microphoneInputs.push_back(*input);
  const bool focusedTransfer = m_transferNotice && inst.keyboardTransferNotice == m_transferNotice->serial;
  const bool focusedConnection = inst.connection
      && !inst.connection->panel.empty()
      && openPanel
      && inst.keyboardCard == inst.connection->actionKey();
  const bool focusedNetwork = inst.network && openPanel && inst.keyboardCard == inst.network->actionKey();
  if (inst.keyboardMode
      && ((inst.keyboardNotification && (!m_notification || m_notification->id != inst.keyboardNotification))
          || (inst.keyboardTransferNotice && (!focusedTransfer || !transferApp(m_transferNotice->source)))
          || (inst.keyboardCard && ((!focusedConnection && !focusedNetwork) || m_transferNotice))
          || (!inst.keyboardNotification
              && !inst.keyboardTransferNotice
              && !inst.keyboardCard
              && !player
              && !microphone
              && !awake
              && !captureActive
              && !cameraActive
              && !inst.captureMenu
              && inst.captureRemaining <= 0
              && downloads.empty()
              && (!cfg.hoverShowDownloads || recentTransfers.empty())
              && !timerActive)))
    releaseKeyboard(inst);
  const bool expansionRequested = inst.hovered || (inst.keyboardMode && !focusedConnection && !focusedNetwork);
  if (player && expansionRequested && (playing || player->playbackStatus == "Paused" || inst.keyboardMode))
    inst.heldMedia = true;
  if (!player || !expansionRequested)
    inst.heldMedia = false;
  const island::Activities availableActivities{
      cfg.hoverShowMedia && player && (playing || inst.heldMedia),
      cfg.hoverShowDownloads && downloadCard,
      cfg.hoverShowTimers && timerActive,
      microphone.has_value(),
      awake.has_value(),
      captureActive,
      cameraActive
  };
  const auto compactActivity = recording ? island::Activity::Capture
      : !announcement.empty()            ? island::Activity::Media
                                         : inst.compactActivity.selected();
  inst.activities.update(expansionRequested, availableActivities, compactActivity, downloads.empty());
  // Keep the existing calendar/timer layout for a lone timer, until switching is useful.
  const auto selected =
      !gCupertino && inst.activities.selected == island::Activity::Timers && !inst.activities.switching
      ? island::Activity::None
      : inst.activities.selected;
  const auto normalView = island::view(
      m_notification.has_value(), m_osd.has_value() && !inst.keyboardMode && !connectionAudioOsd(inst),
      expansionRequested,
      player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds), inst.heldMedia,
      !downloads.empty(), timerActive, cfg.hoverShowMedia, cfg.hoverShowDownloads, selected,
      !announcement.empty() ? island::Activity::Media : inst.compactActivity.selected(),
      m_transferNotice.has_value() && (!inst.keyboardMode || focusedTransfer),
      inst.connection.has_value() && (!inst.keyboardMode || focusedConnection),
      inst.network.has_value() && (!inst.keyboardMode || focusedNetwork), microphone.has_value(), awake.has_value(),
      captureActive, recording, cameraActive, !recentTransfers.empty()
  );
  const auto view = m_notification && m_notification->urgency == Urgency::Critical ? normalView
      : inst.captureRemaining > 0                                                  ? island::View::CaptureCountdown
      : inst.captureMenu                                                           ? island::View::CaptureMenu
                                                                                   : normalView;
  const bool cameraView = view == island::View::Camera;
  const auto& appSessions = cameraView ? m_cameraSessions : m_screenSessions;
  const auto& captureFeedback = cameraView ? inst.cameraFeedback : inst.captureFeedback;
  const bool compactView = view == island::View::Rest
      || view == island::View::Activity
      || view == island::View::DownloadActivity
      || view == island::View::AwakeActivity
      || view == island::View::RecordingActivity
      || view == island::View::TimerActivity;
  const bool expandedView = view == island::View::Calendar
      || view == island::View::Media
      || view == island::View::Downloads
      || view == island::View::Microphone
      || view == island::View::Awake
      || view == island::View::Capture
      || view == island::View::Camera
      || view == island::View::Timers;
  const bool contextualHeader = gCupertino && expandedView && view != island::View::Calendar;
  const bool showSwitcher = !gCupertino && expandedView && inst.activities.switching && availableActivities.count() > 0;
  auto cyclingActivities = availableActivities;
  cyclingActivities.downloads = cfg.hoverShowDownloads && !downloads.empty();
  if (view != island::View::Downloads)
    inst.recentTransfersExpanded = false;
  const bool showRecentTransfers = !gCupertino || downloads.empty() || inst.recentTransfersExpanded;
  // Split Island: with two activities running, the compact capsule shows one and a bubble beside
  // it the other, rather than the activity order hiding the second.
  const auto primaryActivity = view == island::View::Activity ? island::Activity::Media
      : view == island::View::DownloadActivity                ? island::Activity::Downloads
      : view == island::View::TimerActivity                   ? island::Activity::Timers
      : view == island::View::AwakeActivity                   ? island::Activity::Awake
                                                              : island::Activity::None;
  const auto otherActivity = cfg.splitActivities && !recording && primaryActivity != island::Activity::None
      ? island::secondaryActivity(
            {player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds),
             !downloads.empty(), timerActive, false, awake.has_value()},
            island::activityOrder(cfg.activityPriority), primaryActivity
        )
      : island::Activity::None;
  // With nothing else running, two jobs in the downloads lane split between capsule and bubble.
  const bool laneSplit = cfg.splitActivities
      && !recording
      && primaryActivity == island::Activity::Downloads
      && otherActivity == island::Activity::None
      && downloads.size() > 1;
  const auto splitActivity = laneSplit ? island::Activity::Downloads : otherActivity;
  // A third running activity takes a second bubble beside the first.
  const island::Activities runningActivities{
      player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds),
      !downloads.empty(), timerActive, false, awake.has_value()
  };
  island::Activity thirdActivity = island::Activity::None;
  if (!laneSplit && otherActivity != island::Activity::None)
    for (const auto activity : island::activityOrder(cfg.activityPriority))
      if (activity != primaryActivity && activity != otherActivity && runningActivities.contains(activity)) {
        thirdActivity = activity;
        break;
      }
  const std::span<const DownloadProgress> capsuleDownloads =
      laneSplit ? std::span<const DownloadProgress>(downloads).first(1) : std::span<const DownloadProgress>(downloads);
  const std::span<const DownloadProgress> bubbleDownloads = laneSplit
      ? std::span<const DownloadProgress>(downloads).subspan(1)
      : std::span<const DownloadProgress>(downloads);
  // Cupertino focuses an expanded activity on that activity alone, like Apple's Dynamic
  // Island; batteries, unread history and hover widgets stay in the idle (calendar) view.
  // Activity cards keep the tray beside their privacy and unread indicators in the header.
  // Temporary OSDs keep their own content; capture resumes in the following view.
  const bool showExtras = !gCupertino || view == island::View::Calendar;
  const bool chargingOsd = view == island::View::Osd && m_osd && m_osd->kind == OsdKind::Charging;
  const auto batteryList =
      !recording && (compactView || expandedView || chargingOsd || view == island::View::Connection)
      ? batteries(cfg, inst.output)
      : std::vector<island::Battery>{};
  const bool showBattery = compactView && !batteryList.empty() && batteryList.front().compact();
  const auto unreadCount = m_notifications
      ? std::ranges::count_if(m_notifications->history(), [](const auto& entry) { return !entry.seen; })
      : 0;
  bool showUnread = unreadCount > 0
      && !recording
      && (view == island::View::Rest
          || view == island::View::Activity
          || view == island::View::Calendar
          || view == island::View::Media
          || view == island::View::DownloadActivity
          || view == island::View::Downloads
          || view == island::View::Microphone
          || view == island::View::Awake
          || view == island::View::AwakeActivity
          || view == island::View::Capture
          || view == island::View::Camera
          || view == island::View::Timers
          || view == island::View::TimerActivity);
  constexpr float badgeWidth = 24.0F;
  if (gCupertino)
    privacyList = island::displayPrivacy(std::move(privacyList));
  if (!gCupertino && view == island::View::Microphone)
    std::erase_if(privacyList, [](const auto& activity) { return activity.kind == PrivacyCaptureKind::Microphone; });
  if (!gCupertino && view == island::View::Capture)
    std::erase_if(privacyList, [](const auto& activity) { return activity.kind == PrivacyCaptureKind::Screen; });
  if (!gCupertino && cameraView)
    std::erase_if(privacyList, [](const auto& activity) { return activity.kind == PrivacyCaptureKind::Camera; });
  if (!island::showsStatusIcons(view))
    privacyList.clear();
  // Outside the expanded Island capture indicators share one slot, cycling every few seconds;
  // in compact views the unread-notifications bell joins that slot rather than taking its own.
  bool slotBell = false;
  if (!expandedView && !privacyList.empty()) {
    std::vector<std::string> ids;
    for (const auto& activity : privacyList)
      ids.emplace_back(activity.icon());
    if (compactView && showUnread)
      ids.emplace_back("notifications");
    const auto shown = *inst.privacyRotation.pick(ids, island::PrivacyRotation::Clock::now(), inst.badgeHovered);
    slotBell = shown == privacyList.size();
    privacyList = {privacyList[slotBell ? 0 : shown]};
    if (compactView)
      showUnread = false;
  } else if (privacyList.empty()) {
    (void)inst.privacyRotation.pick({}, island::PrivacyRotation::Clock::now());
  }
  const float privacyWidth = privacyList.empty() ? 0 : static_cast<float>(privacyList.size()) * 24 + 8;
  if (!compactView || privacyList.empty())
    inst.slotIcon.clear();
  // A gesture belongs to the track and card where it started.
  if (inst.seeking
      && (view != island::View::Media
          || !player
          || !player->canSeek
          || inst.seekTrackSignature != m_trackSignature
          || inst.seekLengthUs != player->lengthUs)) {
    inst.seeking = false;
    inst.activeSeek = {};
  }
  const auto time = localTime(cfg.clockSeconds ? "%H:%M:%S" : "%H:%M");
  const auto recordingTime = ScreenRecorder::instance().stopping() ? i18n::tr("island.capture.saving")
                                                                   : activityTime(ScreenRecorder::instance().elapsed());
  const auto date = localTime("%A, %d %B");
  const auto position = player ? player->positionUs / 1000000 : 0;
  const auto displayPosition = inst.seeking
      ? static_cast<std::int64_t>(static_cast<double>(inst.seekLengthUs) * inst.seekFraction) / 1000000
      : position;
  const std::string artPath = player
      ? mpris::resolveArtworkSource(
            m_http, m_pendingArtwork, mpris::effectiveArtUrl(*player), [this] { refresh(); }, m_lifetime
        )
      : "";
  // Playback owns the background independently of whichever activity or alert
  // owns the foreground. Keep it through completion, downloads and timers too.
  const std::string flowArt = gCupertino && cfg.mediaGradient && playing ? artPath : "";
  const bool transferAction = view == island::View::TransferNotice && transferApp(m_transferNotice->source);
  const auto downloadRows = std::min(downloads.size(), std::size_t{4});
  std::array<bool, 4> downloadActions{};
  std::array<bool, 3> recentActions{};
  if (view == island::View::Downloads)
    for (std::size_t i = 0; i < downloadRows; ++i)
      downloadActions[i] = transferApp(downloads[i].source);
  if (view == island::View::Downloads)
    for (std::size_t i = 0; i < recentTransfers.size(); ++i)
      recentActions[i] = transferApp(recentTransfers[i].feedback.source);
  std::string actionSignature;
  if (m_notification)
    for (const auto& part : m_notification->actions)
      actionSignature += part + "\n";
  // Hidden events must not reconstruct the visible card. Besides avoiding needless
  // texture work, this preserves a notification while an OSD waits underneath it.
  // Only changed visible status starts a same-view fade. Battery percentages,
  // keyboard focus and another identical completion should remain steady.
  std::string cardPresentation;
  std::string signature = std::to_string(static_cast<int>(view));
  switch (view) {
  case island::View::CaptureMenu:
    signature += std::format(
        "|{}|{}|{}|{}|{}", inst.captureOptions.recording, static_cast<int>(inst.captureOptions.target),
        static_cast<int>(inst.captureOptions.audio), inst.captureOptions.delaySeconds, inst.captureError
    );
    break;
  case island::View::CaptureCountdown:
    signature += inst.captureRecording ? "record" : "screenshot";
    break;
  case island::View::Capture:
  case island::View::Camera:
    for (const auto& [app, started] : appSessions.sessions())
      signature += std::format("|capture:{}:{}", app, started.time_since_epoch().count());
    signature += "|feedback:" + captureFeedback;
    if (cameraView)
      break;
    [[fallthrough]];
  case island::View::RecordingActivity:
    signature += std::format(
        "|recording:{}:{}:{}", recording, ScreenRecorder::instance().sessionId(), ScreenRecorder::instance().stopping()
    );
    break;
  case island::View::AwakeActivity:
  case island::View::Awake:
    break;
  case island::View::Microphone:
    signature += microphone->appNames();
    for (const auto& input : microphoneInputs)
      signature += std::format("|input:{}:{}:{}:{}", input.id, input.name, audioDeviceLabel(input), input.muted);
    break;
  case island::View::Notification:
    signature += std::to_string(m_notification->id)
        + m_notification->appName
        + m_notification->summary
        + m_notification->body
        + actionSignature
        + (inst.expandedNotification == m_notification->id ? "expanded" : "collapsed")
        + formatNotificationTime(m_notification->receivedWallClock.value_or(WallClock::now()))
        + (inst.hovered ? "hovered" : "");
    break;
  case island::View::Osd:
    signature += std::format(
        "{}|{}|{}|{}|{}", static_cast<int>(m_osd->kind), m_osd->value, m_osd->icon, m_osd->progress, m_osd->showProgress
    );
    break;
  case island::View::TransferNotice:
    cardPresentation = std::to_string(static_cast<int>(m_transferNotice->notice));
    signature += std::format(
        "|notice:{}:{}:{}", static_cast<int>(m_transferNotice->notice), m_transferNotice->serial, transferAction
    );
    break;
  case island::View::Connection:
    cardPresentation =
        std::format("{}|{}|{}", inst.connection->name, inst.connection->icon, inst.connection->audioOutput);
    signature += std::format(
        "|connection:{}:{}:{}:{}:{}", inst.connection->actionKey(), inst.connection->name, inst.connection->icon,
        inst.connection->percentage.value_or(-1), inst.connection->audioOutput
    );
    break;
  case island::View::Network:
    cardPresentation = inst.network->title() + "|" + inst.network->detail() + "|" + inst.network->icon();
    signature += inst.network->actionKey() + "|" + inst.network->title() + "|" + inst.network->detail();
    break;
  case island::View::Media:
    signature += std::format(
        "{}|{}|{}|{}|{}", m_trackSignature, playing, artPath, player ? player->title : "",
        player ? joinedArtists(player->artists) : ""
    );
    if (player)
      signature += std::format(
          "|{}|{}|{}|{}|{}|{}|{}|{}", player->lengthUs, player->canPlay, player->canPause, player->canGoPrevious,
          player->canGoNext, player->canSeek, player->identity, player->desktopEntry
      );
    break;
  case island::View::Activity:
    signature += (announcement.empty() ? time : announcement + "\n" + announcementArtist)
        + artPath
        + (playing ? "playing" : "paused");
    if (!announcement.empty())
      cardPresentation = announcement + "\n" + announcementArtist;
    break;
  case island::View::Calendar:
    signature += time + date + (cfg.hoverShowDownloads && !recentTransfers.empty() ? "|recent" : "");
    break;
  case island::View::Rest:
    signature += time;
    break;
  case island::View::TimerActivity:
  case island::View::Timers:
    break;
  case island::View::DownloadActivity:
  case island::View::Downloads:
    signature += view == island::View::Downloads ? std::to_string(player.has_value()) : time;
    signature += laneSplit ? "|lane" : "";
    for (const auto& download : downloads)
      signature += std::format(
          "|{}|{}|{}|{}|{}|{}", download.key, download.name,
          view == island::View::Downloads ? 0L : std::lround(download.progress * 100), download.determinate,
          download.phase, download.icon
      );
    if (view == island::View::Downloads)
      for (std::size_t i = 0; i < downloadRows; ++i)
        signature += std::format(
            "|action:{}:{}:{}", downloadActions[i], downloads[i].source.desktopId, downloads[i].source.wmClass
        );
    if (view == island::View::Downloads)
      for (std::size_t i = 0; i < recentTransfers.size(); ++i)
        signature += std::format("|recent:{}:{}", recentTransfers[i].feedback.serial, recentActions[i]);
    break;
  }
  if (view != island::View::Notification && view != island::View::Osd)
    signature += std::format("|unread:{}|count:{}", showUnread, showUnread && expandedView ? unreadCount : 0);
  signature += std::format(
      "|keyboard:{}|recent-expanded:{}|recording:{}", inst.keyboardMode, inst.recentTransfersExpanded, recording
  );
  if (expandedView)
    signature += std::format(
        "|switcher:{}:{}:{}:{}:{}:{}:{}:{}|media-indicator:{}|live-downloads:{}", showSwitcher,
        availableActivities.media, availableActivities.downloads, availableActivities.timers,
        availableActivities.microphone, availableActivities.awake, availableActivities.capture,
        availableActivities.camera, playing, cyclingActivities.downloads
    );
  if (expandedView || view == island::View::TimerActivity)
    for (const auto& timer : timers)
      signature += std::format(
          "|timer:{}|{}|{}|{}|{}|{}|{}|{}|{}", timer.plugin, countdownTitle(timer), timer.running, timer.active,
          timer.finished, timer.duration, timer.session, timer.url, timer.event && timer.remaining <= 0
      );
  for (const auto& activity : privacyList)
    signature += std::format(
        "|privacy:{}:{}:{}", static_cast<int>(activity.kind), activity.appNames(), activity.includesMicrophone
    );
  if (slotBell)
    signature += "|slot-bell";
  if (showBattery || expandedView)
    for (const auto& battery : batteryList)
      signature += std::format(
          "|battery:{}|{}|{}|{}|{}|{}|{}", battery.id, battery.name, battery.icon, std::lround(battery.percentage),
          static_cast<int>(battery.state), battery.seconds / 60, battery.low
      );
  if (expandedView && cfg.hoverShowUnread && m_notifications)
    signature += "|history:" + std::to_string(m_notifications->changeSerial());
  if (expandedView && inst.hoverWidgets && inst.hoverWidgets->trayOnlyMode() == !showExtras) {
    m_renderContext->makeCurrent(inst.surface->renderTarget());
    const float oldHeight = inst.hoverWidgets->height();
    const float oldContentWidth = inst.hoverWidgets->contentWidth();
    inst.hoverWidgets->updateWidgets(renderer, inst.hoverWidgets->width());
    if (oldHeight != inst.hoverWidgets->height()
        || (inst.hoverWidgets->trayOnlyMode() && oldContentWidth != inst.hoverWidgets->contentWidth()))
      inst.signature.clear();
  }
  const bool outlineTimer = cfg.outerProgressRing
      && !recording
      && timerActive
      && (view == island::View::TimerActivity
          || view == island::View::Timers
          || (expandedView && !inst.activities.switching && cfg.hoverShowTimers));
  const bool outlineDownload = cfg.outerProgressRing
      && !recording
      && !outlineTimer
      && (view == island::View::DownloadActivity || view == island::View::Downloads)
      && !downloads.empty();
  const auto capsuleColors = island::transferColors(capsuleDownloads, MotionService::instance().enabled());
  if (view == island::View::DownloadActivity)
    signature += std::format("|led:{}", capsuleColors.has_value());
  const auto updateOutline = [&] {
    if (!inst.progressOutline)
      return;
    std::optional<float> fraction;
    ColorSpec fill = islandRole(ColorRole::Primary);
    if (outlineTimer) {
      fraction = timers.front().fraction();
      fill = islandTint(countdownTint(timers.front()), ColorRole::Primary);
    } else if (outlineDownload) {
      fill = downloadsPaused(capsuleDownloads) ? islandFixed(kAppleOrange, 1.0F)
                                               : islandTint(kAppleBlue, ColorRole::Primary);
      fraction = downloadFraction(capsuleDownloads);
    }
    inst.progressOutline->update(
        outlineTimer || outlineDownload, fraction, fill, false, islandRole(ColorRole::OnSurface, 0.16F),
        outlineDownload ? capsuleColors : std::nullopt
    );
  };
  updateOutline();
  // Recording starts with two red pulses, then its icon and timer carry the status.
  // Microphone and camera use their coloured icons; desktop sharing keeps its purple glow.
  // Critical notifications and transfer failures use red; confirmed finishes use green.
  const auto recordingStarted = ScreenRecorder::instance().startedAt();
  const bool recordingIntro = recording && Clock::now() - recordingStarted < island::CaptureGlow::kRecordingIntro;
  const bool criticalShown =
      view == island::View::Notification && m_notification && m_notification->urgency == Urgency::Critical;
  const bool recordingSaved = view == island::View::Notification
      && m_notification
      && m_notification->origin == NotificationOrigin::Internal
      && m_notification->category == kRecordingNotificationCategory
      && Clock::now() - m_notification->receivedTime < 5s;
  const auto* connectionBattery = cfg.outerProgressRing ? island::glowingBattery(batteryList) : nullptr;
  const auto updateCaptureGlow = [&] {
    if (inst.captureGlow) {
      const bool notice = view == island::View::TransferNotice;
      const auto noticeColor =
          m_transferNotice && m_transferNotice->notice == island::TransferNotice::Failed ? kAppleRed : kAppleGreen;
      ColorSpec color = islandRole(ColorRole::Error);
      if (m_desktopShared && !recordingIntro && !criticalShown)
        color = island::CaptureGlow::sharingColor();
      if (!m_desktopShared && !recordingIntro && !criticalShown) {
        if (notice || recordingSaved)
          color = islandFixed(recordingSaved ? kAppleGreen : noticeColor, 1.0F);
        else if (connectionBattery) {
          const auto level = island::batteryGlowLevel(connectionBattery->percentage);
          color = islandFixed(
              level == island::BatteryGlowLevel::Green       ? kAppleGreen
                  : level == island::BatteryGlowLevel::Amber ? kAppleOrange
                                                             : kAppleRed,
              1.0F
          );
        }
      }
      inst.captureGlow->update(
          m_desktopShared || recordingIntro || criticalShown || notice || recordingSaved || connectionBattery, color,
          m_desktopShared && !recordingIntro && !criticalShown,
          recordingIntro && !criticalShown ? std::optional{recordingStarted} : std::nullopt
      );
    }
  };
  updateCaptureGlow();
  // One bubble: its activity, the download jobs it stands for, and whether it is the lane split.
  const auto updateBubble = [&](std::size_t index, island::Activity splitActivity,
                                std::span<const DownloadProgress> bubbleDownloads, bool laneSplit) {
    auto& split = inst.splits[index];
    if (!split.bubble)
      return;
    const float s = inst.scale;
    const float d = cfg.height;
    std::optional<float> fraction;
    if (splitActivity == island::Activity::Timers)
      fraction = timers.front().fraction();
    else if (splitActivity == island::Activity::Downloads)
      fraction = downloadFraction(bubbleDownloads);
    const bool paused = splitActivity == island::Activity::Downloads && downloadsPaused(bubbleDownloads);
    const auto colors = splitActivity == island::Activity::Downloads
        ? island::transferColors(bubbleDownloads, MotionService::instance().enabled())
        : std::nullopt;
    // A lone job in the bubble shows its own symbol; a group shows the download arrow.
    const auto bubbleIcon = island::transferGlyph(bubbleDownloads);
    if (index == 0) {
      inst.splitLane = laneSplit;
      inst.splitNext = laneSplit ? bubbleDownloads.front().key : "";
    }
    const auto target = splitActivity == island::Activity::None ? std::string{}
        : laneSplit                                             ? "transfer:" + inst.splitNext
                    : std::format("activity:{}", static_cast<int>(splitActivity));
    if (target != split.target) {
      split.target = target;
      split.pressedTarget.clear();
    }
    // A retracting bubble keeps its last content until it is tucked away.
    if (splitActivity != island::Activity::None) {
      std::string bubbleSignature = std::format("{}|{}|{}|{}", static_cast<int>(splitActivity), d, s, gCupertino);
      if (splitActivity == island::Activity::Media)
        bubbleSignature += "|" + artPath;
      else if (splitActivity == island::Activity::Timers)
        bubbleSignature += "|" + timers.front().plugin + "|" + timers.front().icon;
      else if (splitActivity == island::Activity::Awake)
        bubbleSignature += "|awake";
      else
        bubbleSignature += std::format(
            "|{}|{}|{}|{}|{}", fraction.has_value(), bubbleIcon, laneSplit ? inst.splitNext : "", paused,
            colors.has_value()
        );
      if (bubbleSignature != split.signature) {
        m_renderContext->makeCurrent(inst.surface->renderTarget());
        split.signature = bubbleSignature;
        split.progress = {};
        split.ledRing = nullptr;
        inst.animations.cancel(split.fade);
        split.fade = 0;
        if (split.outgoing)
          (void)split.area->removeChild(split.outgoing);
        split.outgoing = nullptr;
        if (split.content) {
          if (MotionService::instance().enabled() && split.reveal > 0.5F)
            split.outgoing = split.content;
          else
            (void)split.area->removeChild(split.content);
        }
        split.contentFade = split.outgoing ? 0.0F : 1.0F;
        auto content = std::make_unique<Node>();
        content->setSize(d * s, d * s);
        content->setHitTestVisible(false);
        const auto centred = [&](std::unique_ptr<Node> node) {
          node->setPosition((d * s - node->width()) / 2, (d * s - node->height()) / 2);
          return content->addChild(std::move(node));
        };
        const auto symbol = [&](const std::string& name, ColorSpec color, float size = 0.28F) {
          auto node = std::make_unique<Glyph>();
          node->setGlyph(name);
          node->setGlyphSize(std::round(d * size) * s);
          node->setColor(color);
          node->measure(renderer);
          centred(std::move(node));
        };
        bool artShown = false;
        if (splitActivity == island::Activity::Media && !artPath.empty()) {
          // Apple's minimal Now Playing view: the album art, round, filling most of the bubble.
          const float size = std::round(d * 0.62F);
          auto image = std::make_unique<Image>();
          image->setSize(size * s, size * s);
          image->setRadius(size * s / 2);
          image->setFit(ImageFit::Cover);
          if (image->setSourceFile(renderer, artPath, static_cast<int>(std::ceil(size * s * 2.0F)), true, true)) {
            centred(std::move(image));
            artShown = true;
          }
        }
        if (splitActivity == island::Activity::Media && !artShown)
          symbol("music", islandRole(ColorRole::OnSurface));
        if (splitActivity == island::Activity::Awake)
          symbol("caffeine-on", islandTint(kAppleOrange, ColorRole::Primary), 0.38F);
        if (splitActivity == island::Activity::Timers || splitActivity == island::Activity::Downloads) {
          // A progress ring in the activity's colour round the bubble's own rim, the way the
          // capsule's progress traces its edge, with the activity's symbol in the middle.
          const bool timer = splitActivity == island::Activity::Timers;
          const auto tint = paused ? islandFixed(kAppleOrange, 1.0F)
              : timer              ? islandTint(kAppleOrange, ColorRole::Primary)
                                   : islandTint(kAppleBlue, ColorRole::Primary);
          if (colors) {
            auto ring = std::make_unique<island::ProgressOutline>();
            ring->setGeometry(d * s, d * s, d * s / 2, s);
            split.ledRing = ring.get();
            centred(std::move(ring));
          } else {
            auto ring = std::make_unique<DownloadRing>(d * s, 3.0F * s, fraction, tint);
            auto* ringPtr = ring.get();
            centred(std::move(ring));
            if (fraction)
              split.progress = [ringPtr](float value) { ringPtr->setProgress(value); };
          }
          symbol(timer ? timers.front().icon : bubbleIcon, tint, 0.34F);
        }
        split.content = split.area->addChild(std::move(content));
        if (split.outgoing)
          split.fade = inst.animations.animate(
              0.0F, 1.0F, Motion::contentMs, Motion::dismiss,
              [this, &inst, &split](float value) {
                split.contentFade = value;
                geometry(inst);
              },
              [&split] {
                if (split.outgoing)
                  (void)split.area->removeChild(split.outgoing);
                split.outgoing = nullptr;
                split.fade = 0;
              }
          );
        split.area->setTooltip(
            laneSplit && bubbleDownloads.size() == 1
                ? bubbleDownloads.front().name
                : i18n::tr(
                      splitActivity == island::Activity::Media        ? "island.split.media"
                          : splitActivity == island::Activity::Awake  ? "island.split.awake"
                          : splitActivity == island::Activity::Timers ? "island.split.timers"
                                                                      : "island.split.downloads"
                  )
        );
      }
      if (split.progress && fraction)
        split.progress(*fraction);
      if (split.ledRing)
        split.ledRing->update(
            true, 1.0F, islandTint(kAppleBlue, ColorRole::Primary), false, islandRole(ColorRole::OnSurface, 0.16F),
            colors
        );
    }
    const bool shown = splitActivity != island::Activity::None;
    const bool wasShown = split.activity != island::Activity::None;
    split.activity = splitActivity;
    split.area->setHitTestVisible(shown && split.reveal > 0.5F);
    if (shown == wasShown)
      return;
    inst.animations.cancel(split.morph);
    split.morph = 0;
    if (!MotionService::instance().enabled()) {
      split.reveal = shown ? 1.0F : 0.0F;
      geometry(inst);
      return;
    }
    const float from = split.reveal;
    split.morph = inst.animations.animate(
        from, shown ? 1.0F : 0.0F, shown ? kSplitOutMs : kSplitInMs, shown ? Easing::EaseOutBack : Motion::dismiss,
        [this, &inst, &split](float value) {
          split.reveal = value;
          geometry(inst);
        },
        [&split] { split.morph = 0; }
    );
  };
  const auto updateSplit = [&] {
    updateBubble(0, splitActivity, bubbleDownloads, laneSplit);
    updateBubble(1, thirdActivity, std::span<const DownloadProgress>(downloads), false);
  };
  updateSplit();
  // Playback/artwork changes must also reach retained cards without rebuilding
  // their buttons, focus or notification contents.
  updateFlow(inst, flowArt);
  if (signature == inst.signature && inst.root) {
    if (inst.downloadLedRing)
      inst.downloadLedRing->update(
          true, 1.0F, islandTint(kAppleBlue, ColorRole::Primary), false, islandRole(ColorRole::OnSurface, 0.16F),
          capsuleColors
      );
    if (inst.captureCountdownLabel)
      inst.captureCountdownLabel->setText(
          i18n::tr("island.capture-menu.countdown", "seconds", std::to_string(inst.captureRemaining))
      );
    if ((recording && inst.recordingLabel)
        || !inst.captureLabels.empty()
        || (inst.awakeLabel && awake)
        || !inst.timerUi.empty()
        || !inst.downloadUi.empty()
        || !inst.recentTransferUi.empty()
        || (view == island::View::Media && player))
      m_renderContext->makeCurrent(inst.surface->renderTarget());
    geometry(inst);
    if (inst.awakeLabel && awake) {
      inst.awakeLabel->setText(activityTime(*awake));
      inst.awakeLabel->measure(renderer);
    }
    // Timer ticks must not rebuild the stop action between pointer press and release.
    if (recording && inst.recordingLabel) {
      inst.recordingLabel->setText(recordingTime);
      inst.recordingLabel->measure(renderer);
    }
    for (const auto& [app, label] : inst.captureLabels) {
      if (const auto elapsed = appSessions.elapsed(app, island::CaptureSessions::Clock::now())) {
        label->setText(i18n::tr("island.capture.elapsed", "time", activityTime(*elapsed)));
        label->measure(renderer);
      }
    }
    for (const auto& ui : inst.timerUi) {
      const auto timer = std::ranges::find(timers, ui.plugin, &island::Countdown::plugin);
      if (timer == timers.end())
        continue;
      ui.label->setText(ui.eventStatus ? eventStatus(*timer) : countdownTime(*timer));
      ui.label->measure(renderer);
      ui.setFraction(timer->fraction());
    }
    for (const auto& ui : inst.downloadUi) {
      const auto download = std::ranges::find(downloads, ui.key, &DownloadProgress::key);
      if (download == downloads.end())
        continue;
      ui.percentage->setText(std::format("{}%", std::lround(download->progress * 100)));
      ui.percentage->measure(renderer);
      if (ui.progress)
        ui.progress->setProgress(static_cast<float>(download->progress));
    }
    for (const auto& [serial, age] : inst.recentTransferUi) {
      if (const auto* entry = m_recentTransfers.find(serial)) {
        age->setText(recentTransferAge(*entry));
        age->measure(renderer);
      }
    }
    // Keep the title's marquee alive while the playback position advances.
    if (view == island::View::Media && player) {
      if (inst.seekProgress)
        inst.seekProgress->setProgress(
            inst.seeking ? inst.seekFraction
                : player->lengthUs > 0
                ? std::clamp(static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs), 0.0F, 1.0F)
                : 0.0F
        );
      if (inst.mediaPosition) {
        inst.mediaPosition->setText(std::format("{}:{:02}", displayPosition / 60, displayPosition % 60));
        inst.mediaPosition->setColor(inst.seeking ? islandRole(ColorRole::Primary) : muted);
        inst.mediaPosition->measure(renderer);
      }
    }
    return;
  }
  m_renderContext->makeCurrent(inst.surface->renderTarget());
  inst.signature = signature;
  const float s = inst.scale;
  auto [w, h] = island::size(
      view, cfg.height, cfg.clockSize, cfg.clockSeconds, cfg.calendarLabels != IslandCalendarLabels::Initials,
      cfg.mediaArtworkSize, !announcement.empty()
  );
  const bool paddedCard = gCupertino
      && (expandedView
          || view == island::View::Notification
          || view == island::View::CaptureMenu
          || view == island::View::CaptureCountdown);
  if (paddedCard)
    w = island::cardWidth(cfg.compactLayout);
  if (showSwitcher)
    w = std::max({w, 360.0F, 32.0F + 88.0F * static_cast<float>(availableActivities.count())});
  w = island::batteryWidth(w, view, showBattery, showUnread);
  if (recording)
    w = std::max(w, 250.0F);
  if (compactView)
    w += 2 * privacyWidth;
  w = std::min(w, static_cast<float>(inst.surface->width()) / s - 16);
  // Existing inner groups start 12 px in. Add an outer gutter so their filled
  // backgrounds clear the large Island curve by 20 px compact / 24 px comfortable.
  const float cardGutter = paddedCard ? (cfg.compactLayout ? 8.0F : 12.0F) : 0.0F;
  w = std::max(1.0F, w - 2 * cardGutter);
  if (!inst.root) {
    inst.root = std::make_unique<Node>();
    inst.root->setAnimationManager(&inst.animations);
    // Behind the capsule, so only the part of its halo outside the edge shows.
    auto glow = std::make_unique<island::CaptureGlow>();
    inst.captureGlow = static_cast<island::CaptureGlow*>(inst.root->addChild(std::move(glow)));
    // Behind the capsule too, so the split bubbles slide out from under it; the second is added
    // first, so it sits behind the first and slides out from under that.
    for (std::size_t index = inst.splits.size(); index-- > 0;) {
      auto& split = inst.splits[index];
      auto bubble = std::make_unique<Box>();
      bubble->setFill(islandRole(ColorRole::Surface));
      bubble->setClipChildren(true);
      bubble->setVisible(false);
      auto area = std::make_unique<InputArea>();
      area->setHitShape(InputArea::HitShape::Circle);
      area->setOnEnter([&inst, &split](const InputArea::PointerData&) {
        // Reaching for a bubble must not expand the capsule beside it.
        split.hovered = true;
        inst.enter.stop();
      });
      area->setOnLeave([this, &inst, &split] {
        split.hovered = false;
        if (inst.inside && !inst.hovered && !inst.suppressHover)
          inst.enter.start(std::chrono::milliseconds(inst.config.hoverOpenDelayMs), [this, &inst] {
            if (inst.inside && !inst.badgeHovered && !inst.splitHovered() && !cardActionKey(inst).has_value()) {
              inst.hovered = true;
              refresh();
            }
          });
      });
      area->setOnPress([&split](const InputArea::PointerData& data) {
        if (data.pressed)
          split.pressedTarget = split.target;
      });
      area->setOnCancel([&split] { split.pressedTarget.clear(); });
      area->setOnClick([this, &inst, &split, index](const InputArea::PointerData&) {
        const bool activate = !split.target.empty() && split.pressedTarget == split.target;
        split.pressedTarget.clear();
        if (!activate || split.activity == island::Activity::None)
          return;
        if (index == 0 && inst.splitLane)
          inst.splitLead = inst.splitNext;
        else
          inst.compactActivity.promote(split.activity);
        refresh();
      });
      split.area = static_cast<InputArea*>(bubble->addChild(std::move(area)));
      split.bubble = static_cast<Box*>(inst.root->addChild(std::move(bubble)));
    }
    auto box = std::make_unique<Box>();
    box->setFill(islandRole(ColorRole::Surface));
    box->setClipChildren(true);
    inst.background = box.get();
    // First child of the capsule, so rebuilt content always draws over it.
    auto flow = std::make_unique<Image>();
    flow->setFit(ImageFit::Cover);
    flow->setHitTestVisible(false);
    flow->setVisible(false);
    inst.flowImage = static_cast<Image*>(box->addChild(std::move(flow)));
    inst.root->addChild(std::move(box));
    auto outline = std::make_unique<island::ProgressOutline>();
    inst.progressOutline = static_cast<island::ProgressOutline*>(inst.root->addChild(std::move(outline)));
    updateOutline();
    updateCaptureGlow();
    inst.surface->setSceneRoot(inst.root.get());
    inst.input.setSceneRoot(inst.root.get());
    inst.width = w;
    inst.height = h;
    updateSplit();
  }
  // A newly created layer also needs its first frame, including after reload.
  updateFlow(inst, flowArt);
  // Critical notifications pulse with the capture glow (above) rather than taking an outline.
  inst.background->clearBorder();
  inst.root->setSize(static_cast<float>(inst.surface->width()), static_cast<float>(inst.surface->height()));
  const auto keyboardFocus = inst.input.captureTabFocus();
  const float activityOffset =
      inst.activityScroll && inst.previousView == view ? inst.activityScroll->scrollOffset() : 0;
  const bool viewChanged = inst.previousView != view;
  inst.captureOptionResize = !viewChanged && view == island::View::CaptureMenu;
  const bool cardChanged = !viewChanged && !cardPresentation.empty() && cardPresentation != inst.cardPresentation;
  inst.cardPresentation = std::move(cardPresentation);
  inst.previousView = view;
  inst.activityScroll = nullptr;
  inst.badgeHovered = false;
  inst.pressedAction.clear();
  const bool showMediaStatus =
      view == island::View::Activity && (gCupertino || (!showUnread && !showBattery && privacyList.empty()));
  const bool showVisualizer = playing && (showMediaStatus || (gCupertino && view == island::View::Media));
  std::unique_ptr<Node> retainedWidgets;
  if (expandedView && inst.hoverWidgets && inst.hoverWidgets->trayOnlyMode() == !showExtras)
    retainedWidgets = inst.hoverWidgets->parent()->removeChild(inst.hoverWidgets);
  inst.hoverWidgets = nullptr;
  std::unique_ptr<Node> retainedVisualizer;
  if (inst.visualizer && showVisualizer)
    retainedVisualizer = inst.content->removeChild(inst.visualizer);
  inst.visualizer = nullptr;
  for (auto* meter : inst.microphoneMeters)
    meter->stop();
  inst.microphoneMeters.clear();
  if (inst.content) {
    auto previous = inst.background->removeChild(inst.content);
    // Option changes keep the capture menu readable while its height springs
    // between Screenshot and Record. Fading the entire form flashes it dark.
    if (!MotionService::instance().enabled() || inst.skipCrossfade || inst.captureOptionResize)
      clearCrossfade(inst);
    else if ((viewChanged || cardChanged) && previous)
      crossfadeOut(inst, std::move(previous), cardChanged);
  }
  inst.skipCrossfade = false;
  auto content = std::make_unique<Node>();
  content->setSize(w * s, h * s);
  inst.content = content.get();
  inst.background->addChild(std::move(content));
  inst.actions.clear();
  inst.seek = {};
  inst.seekProgress = nullptr;
  inst.mediaPosition = nullptr;
  inst.recordingLabel = nullptr;
  inst.captureCountdownLabel = nullptr;
  inst.captureLabels.clear();
  inst.awakeLabel = nullptr;
  inst.timerUi.clear();
  inst.downloadUi.clear();
  inst.recentTransferUi.clear();
  inst.downloadLedRing = nullptr;
  Node* canvas = inst.content;

  const auto label = [&](std::string text, float x, float y, float width, float size,
                         ColorSpec color = islandRole(ColorRole::OnSurface), bool center = false, int lines = 1,
                         FontWeight weight = FontWeight::Normal, bool scroll = false) {
    auto node = std::make_unique<Label>();
    node->setText(text);
    node->setFontSize(size * s);
    node->setFontWeight(weight);
    node->setFontFamily(m_config->config().shell.fontFamily);
    node->setColor(color);
    node->setMaxWidth(width * s);
    node->setMinWidth(width * s);
    node->setMaxLines(lines);
    node->setTextAlign(center ? TextAlign::Center : TextAlign::Start);
    node->setAutoScroll(scroll);
    node->setAutoScrollSpeed(24.0F * s);
    node->setAnimationManager(&inst.animations);
    node->measure(renderer);
    node->setPosition(x * s, y * s);
    auto* result = node.get();
    canvas->addChild(std::move(node));
    return result;
  };
  const auto glyph = [&](const std::string& name, float x, float y, float size,
                         ColorSpec color = islandRole(ColorRole::OnSurface)) {
    auto node = std::make_unique<Glyph>();
    node->setGlyph(name);
    node->setGlyphSize(size * s);
    node->setColor(color);
    node->measure(renderer);
    node->setPosition(x * s, y * s);
    canvas->addChild(std::move(node));
  };
  constexpr float statusIconSize = 32;
  // Measure the whole status group so short titles and translated labels share
  // the same centred layout, with an optional second line beneath the title.
  const auto statusText = [&](const std::string& title, const std::string& detail) {
    constexpr float gap = 12, padding = 20;
    const auto& family = m_config->config().shell.fontFamily;
    float measuredWidth =
        renderer.measureText(title, 15 * s, FontWeight::SemiBold, 0, 1, TextAlign::Start, family).width;
    if (!detail.empty())
      measuredWidth = std::max(
          measuredWidth, renderer.measureText(detail, 12 * s, FontWeight::Normal, 0, 1, TextAlign::Start, family).width
      );
    const float textWidth =
        std::min(std::max(0.0F, w - 2 * padding - statusIconSize - gap), std::ceil(measuredWidth / s));
    const float x = (w - statusIconSize - gap - textWidth) / 2;
    auto* name = label(title, x + statusIconSize + gap, 0, textWidth, 15, foreground, true, 1, FontWeight::SemiBold);
    auto* state = detail.empty() ? nullptr : label(detail, x + statusIconSize + gap, 0, textWidth, 12, muted, true);
    const float block = name->height() + (state ? 2 * s + state->height() : 0);
    name->setPosition(name->x(), (h * s - block) / 2);
    if (state)
      state->setPosition(state->x(), name->y() + name->height() + 2 * s);
    return x;
  };
  const auto sectionCard = [&](float top, float bottom) {
    if (bottom - top <= 6 || contextualHeader)
      return;
    auto card = std::make_unique<Box>();
    card->setCardStyle(s, 1.0F, Style::cardBordersEnabled());
    if (gCupertino) {
      // Grouped content on black: a faint lift rather than the palette's card surface.
      card->setFill(islandRole(ColorRole::SurfaceVariant));
      card->setBorder(islandRole(ColorRole::Outline, 0.5F), Style::borderWidth);
    }
    card->setPosition(12 * s, (top + 2) * s);
    card->setSize((w - 24) * s, (bottom - top - 6) * s);
    card->setHitTestVisible(false);
    card->setZIndex(-1);
    canvas->addChild(std::move(card));
  };
  const auto progress = [&](float value, float x, float y, float width, float height = Style::sliderTrackHeight) {
    auto node = std::make_unique<ProgressBar>();
    node->setTrack(islandRole(ColorRole::OnSurface, 0.16F));
    node->setFill(islandRole(ColorRole::Primary));
    node->setSize(width * s, height * s);
    node->setRadius(height * s / 2.0F);
    node->setProgress(std::clamp(value, 0.0F, 1.0F));
    node->setPosition(x * s, y * s);
    auto* ptr = node.get();
    canvas->addChild(std::move(node));
    return ptr;
  };
  const auto action = [&](float x, float y, float width, float height, std::string id, std::function<void()> cb) {
    inst.actions.push_back({x, y, width, height, std::move(id), std::move(cb)});
  };
  const auto control = [&](float x, float y, float width, float height, const std::string& text,
                           const std::string& icon, const std::string& tooltip, float iconSize, bool available,
                           std::function<void()> cb, float fontSize = Style::fontSizeCaption, float padding = -1) {
    auto node = std::make_unique<Button>();
    setIslandVariant(node.get(), ButtonVariant::Ghost);
    if (!text.empty())
      node->setText(text);
    if (!icon.empty())
      node->setGlyph(icon);
    if (!text.empty())
      node->setFontSize(fontSize * s);
    if (!icon.empty())
      node->setGlyphSize(iconSize * s);
    node->setPadding(padding >= 0 ? padding * s : (text.empty() ? 0 : 8 * s));
    node->setRadius(Style::scaledRadiusMd(s));
    node->setMinWidth(width * s);
    node->setMaxWidth(width * s);
    node->setControlHeight(height * s);
    node->setEnabled(available);
    node->setTooltip(tooltip);
    node->setOnClick(std::move(cb));
    node->inputArea()->setTabFocusKey(icon.empty() ? text : icon);
    auto* result = node.get();
    canvas->addChild(std::move(node));
    result->setSize(width * s, height * s);
    result->layout(renderer);
    result->setPosition(x * s, y * s);
    return result;
  };
  // Cupertino controls: grey capsules for text actions, and round symbol buttons, optionally
  // tinted with an activity colour (orange pause for timers), as in Apple's Live Activities.
  const auto pill = [&](Button* button) {
    if (!gCupertino)
      return;
    setIslandVariant(button, ButtonVariant::Default);
    button->setRadius(button->height() / 2);
  };
  const auto roundButton = [&](Button* button, std::optional<Color> tint = std::nullopt) {
    if (!gCupertino)
      return;
    button->setRadius(button->width() / 2);
    if (!tint) {
      setIslandVariant(button, ButtonVariant::Default);
      return;
    }
    const auto state = [&](float bg, float text) {
      return Button::ButtonStateColors{
          .bg = islandFixed(*tint, bg), .border = islandFixed(*tint, 0), .label = islandFixed(*tint, text)
      };
    };
    button->setCustomPalette(
        {.normal = state(0.24F, 1.0F),
         .hover = state(0.32F, 1.0F),
         .pressed = state(0.4F, 1.0F),
         .disabled = state(0.12F, 0.4F),
         .selected = std::nullopt}
    );
  };
  // A round tinted badge behind a symbol, leading a row (downloads, unread notifications).
  const auto leadingBadge = [&](const std::string& icon, float x, float y, float size, Color tint, ColorRole theme) {
    auto disc = std::make_unique<Box>();
    disc->setFill(islandTint(tint, theme, 0.22F));
    disc->setRadius(size * s / 2);
    disc->setSize(size * s, size * s);
    disc->setPosition(x * s, y * s);
    disc->setHitTestVisible(false);
    canvas->addChild(std::move(disc));
    const float glyphSize = std::round(size * 0.5F);
    glyph(icon, x + (size - glyphSize) / 2, y + (size - glyphSize) / 2, glyphSize, islandTint(tint, theme));
  };
  const auto panel = [this, &inst](const std::string& name) {
    if (inst.keyboardMode)
      releaseKeyboard(inst);
    inst.suppressHover = true;
    inst.hovered = false;
    inst.heldMedia = false;
    if (openPanel)
      openPanel(inst.output, name);
    refresh();
  };
  const auto openPrivacyActivity = [this, &inst](island::Activity activity) {
    inst.enter.stop();
    inst.hovered = true;
    inst.suppressHover = false;
    inst.activities.selected = activity;
    m_osd.reset();
    m_osdTimeout.stop();
    refresh();
  };
  const auto openMicrophone = [openPrivacyActivity] { openPrivacyActivity(island::Activity::Microphone); };
  const auto openCapture = [openPrivacyActivity] { openPrivacyActivity(island::Activity::Capture); };
  const auto openCamera = [openPrivacyActivity] { openPrivacyActivity(island::Activity::Camera); };
  // The notification's own image, its icon (a file or theme name), or its app's icon by desktop
  // entry or name, in that order, as a small rounded image.
  const auto notificationIcon = [&](const Notification& note, float x, float y, float size) {
    auto image = std::make_unique<Image>();
    image->setSize(size * s, size * s);
    image->setRadius(size * s * 0.25F);
    image->setFit(ImageFit::Cover);
    image->setPosition(x * s, y * s);
    const int pixels = static_cast<int>(std::ceil(size * s * 2.0F));
    bool loaded = false;
    if (note.imageData
        && note.imageData->width > 0
        && note.imageData->height > 0
        && !note.imageData->data.empty()
        && note.imageData->bitsPerSample == 8
        && (note.imageData->channels == 3 || note.imageData->channels == 4)) {
      const auto& raw = *note.imageData;
      loaded = image->setSourceRaw(
          renderer, raw.data.data(), raw.data.size(), raw.width, raw.height, raw.rowStride,
          raw.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA, true
      );
    }
    std::vector<std::string> names;
    if (note.icon && !note.icon->empty()) {
      std::string icon = *note.icon;
      if (icon.starts_with("file://"))
        icon.erase(0, 7);
      if (icon.front() == '/' && ::access(icon.c_str(), R_OK) == 0)
        loaded = loaded || image->setSourceFile(renderer, icon, pixels, true);
      else if (icon.front() != '/' && !icon.starts_with("noctalia-glyph:"))
        names.push_back(icon);
    }
    if (note.desktopEntry && !note.desktopEntry->empty())
      names.push_back(*note.desktopEntry);
    if (!note.appName.empty()) {
      std::string lower = note.appName;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
      std::ranges::replace(lower, ' ', '-');
      names.push_back(lower);
    }
    for (const auto& name : names) {
      if (loaded)
        break;
      const auto& path = m_iconResolver.resolve(name, pixels);
      loaded = !path.empty() && image->setSourceFile(renderer, path, pixels, true);
    }
    if (loaded)
      canvas->addChild(std::move(image));
    return loaded;
  };
  const auto artwork = [&](float x, float y, float size) {
    auto image = std::make_unique<Image>();
    image->setSize(size * s, size * s);
    image->setRadius(Style::scaledRadiusLg(s));
    image->setFit(ImageFit::Cover);
    image->setPosition(x * s, y * s);
    // Crop before downsampling so wide thumbnails retain a full-resolution
    // square. Keep extra detail for fractional scaling; Image applies buffer scale.
    const int decodeSize = static_cast<int>(std::ceil(size * s * 2.0F));
    if (!artPath.empty() && image->setSourceFile(renderer, artPath, decodeSize, true, true))
      inst.content->addChild(std::move(image));
    else
      glyph("disc", x + 4, y + 4, size - 8);
  };

  if (view == island::View::CaptureMenu) {
    const auto tr = [](const std::string& key) { return i18n::tr("island.capture-menu." + key); };
    glyph("screenshot", 22, 19, 22, foreground);
    label(tr("title"), 56, 17, 130, 16, foreground, false, 1, FontWeight::SemiBold);
    label(tr("keyboard-hint"), 190, 21, w - 212, 11, muted);
    const auto option = [&](float x, float y, float width, const std::string& key, const std::string& text, bool active,
                            std::function<void()> update) {
      auto* button = control(
          x, y, width, 32, text, "", "", 0, true,
          [this, &inst, update] {
            update();
            inst.captureError.clear();
            refresh();
          },
          13
      );
      button->inputArea()->setTabFocusKey("capture-menu-" + key);
      // Rebuilding the options must keep focus on the option the user just chose.
      button->inputArea()->setRetainsFocusOnPointerRelease(true);
      // A focus ring must not look like a second selected capture mode.
      button->setVariant(ButtonVariant::Default);
      const auto variant = active ? ButtonVariant::TabActive : ButtonVariant::Tab;
      auto optionPalette = gCupertino ? islandButtonPalette(variant) : Button::defaultPalette(variant);
      // Only the selected option gets a fill. Focus retains its outline, so Screenshot
      // cannot look selected while Record is still showing its audio controls.
      if (!active)
        optionPalette.hover.bg = optionPalette.normal.bg;
      button->setCustomPalette(std::move(optionPalette));
      button->setRadius(8 * s);
    };
    const auto segments = [&](float x, float y, float width, float height, float radius) {
      auto track = std::make_unique<Box>();
      track->setPosition(x * s, y * s);
      track->setSize(width * s, height * s);
      track->setFill(islandRole(ColorRole::OnSurface, 0.07F));
      track->setRadius(radius * s);
      track->setHitTestVisible(false);
      track->setZIndex(-1);
      canvas->addChild(std::move(track));
    };
    segments(22, 50, w - 44, 40, 12);
    const float half = (w - 52) / 2;
    option(26, 54, half, "screenshot", tr("screenshot"), !inst.captureOptions.recording, [&inst] {
      inst.captureOptions.recording = false;
    });
    option(26 + half, 54, half, "record", tr("record"), inst.captureOptions.recording, [&inst] {
      inst.captureOptions.recording = true;
      if (inst.captureOptions.target == capture::Target::Window)
        inst.captureOptions.target = capture::Target::Region;
    });
    label(tr("area"), 22, 107, 78, 13, muted);
    segments(98, 98, w - 118, 36, 10);
    const int targetCount = inst.captureOptions.recording ? 2 : 3;
    const float targetWidth = (w - 122 - 4 * static_cast<float>(targetCount - 1)) / static_cast<float>(targetCount);
    int targetIndex = 0;
    for (const auto& [target, key] :
         {std::pair{capture::Target::Region, "region"}, std::pair{capture::Target::Window, "window"},
          std::pair{capture::Target::Monitor, "monitor"}}) {
      if (inst.captureOptions.recording && target == capture::Target::Window)
        continue;
      option(
          100 + static_cast<float>(targetIndex++) * (targetWidth + 4), 100, targetWidth, key, tr(key),
          inst.captureOptions.target == target, [&inst, target] { inst.captureOptions.target = target; }
      );
    }
    label(tr("delay"), 22, 149, 78, 13, muted);
    segments(98, 140, w - 118, 36, 10);
    const float delayWidth = (w - 134) / 4;
    int index = 0;
    for (const int seconds : {0, 3, 5, 10}) {
      option(
          100 + static_cast<float>(index++) * (delayWidth + 4), 142, delayWidth, "delay-" + std::to_string(seconds),
          seconds ? std::to_string(seconds) + "s" : tr("off"), inst.captureOptions.delaySeconds == seconds,
          [&inst, seconds] { inst.captureOptions.delaySeconds = seconds; }
      );
    }
    float bottom = 188;
    if (inst.captureOptions.recording) {
      label(tr("audio"), 22, 191, 78, 13, muted);
      segments(98, 182, w - 118, 36, 10);
      const float audioWidth = (w - 130) / 3;
      int audioIndex = 0;
      for (const auto& [audio, key] :
           {std::pair{capture::RecordingAudio::Off, "off"}, std::pair{capture::RecordingAudio::Desktop, "desktop"},
            std::pair{capture::RecordingAudio::Microphone, "microphone"}})
        option(
            100 + static_cast<float>(audioIndex++) * (audioWidth + 4), 184, audioWidth, std::string("audio-") + key,
            tr(key), inst.captureOptions.audio == audio, [&inst, audio] { inst.captureOptions.audio = audio; }
        );
      bottom += 42;
    }
    if (!inst.captureError.empty()) {
      auto* error = label(inst.captureError, 22, bottom, w - 44, 12, islandTint(kAppleRed, ColorRole::Error), false, 3);
      bottom += error->height() / s + 12;
    }
    auto* cancel =
        control(22, bottom, 94, 34, tr("cancel"), "", "", 0, true, [this, &inst] { releaseKeyboard(inst); }, 13);
    pill(cancel);
    cancel->inputArea()->setTabFocusKey("capture-menu-cancel");
    auto* start = control(
        124, bottom, w - 146, 34,
        tr(inst.captureOptions.target == capture::Target::Window        ? "select-window"
               : inst.captureOptions.target == capture::Target::Monitor ? "select-monitor"
                                                                        : "select-region"),
        "", "", 0, true, [this, &inst] {
          const auto options = inst.captureOptions;
          inst.skipCrossfade = true;
          releaseKeyboard(inst);
          if (beginCapture) {
            const auto error = beginCapture(options);
            if (!error.empty()) {
              inst.captureError = error;
              openCaptureMenu(inst.output);
            }
          }
        }
    );
    start->setFontSize(13 * s);
    setIslandVariant(start, ButtonVariant::Primary);
    roundButton(start, inst.captureOptions.recording ? kAppleRed : kAppleBlue);
    start->setRadius(start->height() / 2);
    start->inputArea()->setTabFocusKey("capture-menu-start");
    h = bottom + 50;
  } else if (view == island::View::CaptureCountdown) {
    glyph(
        inst.captureRecording ? "player-record-filled" : "screenshot", 22, 18, 24,
        islandTint(inst.captureRecording ? kAppleRed : kAppleBlue, ColorRole::Primary)
    );
    label(
        i18n::tr(
            inst.captureRecording ? "island.capture-menu.starting-recording" : "island.capture-menu.taking-screenshot"
        ),
        58, 15, w - 80, 14, foreground, false, 1, FontWeight::SemiBold
    );
    inst.captureCountdownLabel = label(
        i18n::tr("island.capture-menu.countdown", "seconds", std::to_string(inst.captureRemaining)), 58, 36, w - 80, 12,
        muted
    );
    auto* cancel = control(22, 65, w - 44, 30, i18n::tr("island.capture-menu.cancel"), "", "", 0, true, [this] {
      if (cancelCapture)
        cancelCapture();
    });
    pill(cancel);
    cancel->inputArea()->setTabFocusKey("capture-countdown-cancel");
    h = 108;
  } else if (view == island::View::DownloadActivity || view == island::View::TimerActivity) {
    const bool timerView = view == island::View::TimerActivity;
    const bool paused = !timerView && downloadsPaused(capsuleDownloads);
    const auto fraction = timerView ? std::optional{timers.front().fraction()} : downloadFraction(capsuleDownloads);
    DownloadRing* ringPtr = nullptr;
    if (!(outlineTimer || outlineDownload) && !timerView && capsuleColors) {
      auto ring = std::make_unique<island::ProgressOutline>();
      ring->setGeometry(36 * s, 36 * s, 18 * s, s);
      ring->setPosition(14 * s, (cfg.height - 36) * s / 2);
      ring->update(
          true, 1.0F, islandTint(kAppleBlue, ColorRole::Primary), false, islandRole(ColorRole::OnSurface, 0.16F),
          capsuleColors
      );
      inst.downloadLedRing = ring.get();
      canvas->addChild(std::move(ring));
    } else if (!(outlineTimer || outlineDownload)) {
      auto ring = std::make_unique<DownloadRing>(
          36 * s, 2.5F * s, fraction,
          paused ? islandFixed(kAppleOrange, 1.0F) : islandTint(kAppleBlue, ColorRole::Primary)
      );
      ringPtr = ring.get();
      ring->setPosition(14 * s, (cfg.height - 36) * s / 2);
      canvas->addChild(std::move(ring));
    }
    const auto downloadIcon = island::transferGlyph(capsuleDownloads);
    // A lone script activity names itself where the clock would be, like a Live Activity.
    const bool scriptTitle = !timerView
        && capsuleDownloads.size() == 1
        && !capsuleDownloads.front().icon.empty()
        && !capsuleDownloads.front().name.empty();
    glyph(
        timerView ? timers.front().icon : downloadIcon, 23, (cfg.height - 18) / 2, 18,
        paused ? islandFixed(kAppleOrange, 1.0F) : islandRole(ColorRole::Primary)
    );
    const float inset = (showUnread ? 95.0F : 70.0F) + privacyWidth;
    const float available = std::max(1.0F, w - 2 * inset);
    const auto clockText = timerView ? countdownTime(timers.front())
        : paused                     ? i18n::tr("island.downloads.paused")
        : scriptTitle                ? capsuleDownloads.front().name
                                     : time;
    const float textSize = scriptTitle || paused ? std::min(cfg.clockSize, 16.0F) : cfg.clockSize;
    const auto metrics = renderer.measureText(
        clockText, textSize * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
    );
    const float clockSize =
        scriptTitle && !paused ? textSize : textSize * std::min(1.0F, available * s / std::max(1.0F, metrics.width));
    auto* clockLabel = label(clockText, inset, 0, available, clockSize, foreground, true);
    if (timerView)
      inst.timerUi.push_back({timers.front().plugin, clockLabel, [ringPtr](float value) {
                                if (ringPtr)
                                  ringPtr->setProgress(value);
                              }});
    clockLabel->setPosition(
        inset * s,
        std::clamp(
            (cfg.height * s - clockLabel->height()) / 2 + cfg.clockOffset * s, 0.0F,
            std::max(0.0F, cfg.height * s - clockLabel->height())
        )
    );
    if (timerView && !showBattery && privacyList.empty() && !timers.front().event) {
      glyph(
          timers.front().running        ? "media-pause"
              : timers.front().finished ? "check"
                                        : "media-play",
          w - (showUnread ? 80 : 46), (cfg.height - 18) / 2, 18, muted
      );
    } else if (
        !timerView
        && !showBattery
        && privacyList.empty()
        && (capsuleDownloads.size() > 1 || capsuleDownloads.front().determinate)
    ) {
      const auto value = capsuleDownloads.size() == 1
          ? std::format("{}%", std::lround(capsuleDownloads.front().progress * 100))
          : std::to_string(capsuleDownloads.size());
      auto* status = label(value, w - (showUnread ? 91 : 70), 0, 50, 12, muted, true);
      status->setPosition(status->x(), (cfg.height * s - status->height()) / 2);
    }
    action(0, 0, w, cfg.height, "downloads", [this, &inst] {
      inst.suppressHover = false;
      inst.hovered = true;
      refresh();
    });
  } else if (view == island::View::Downloads) {
    // Script activities share this card; it is "In Progress" unless every row is a download.
    const bool onlyDownloads = std::ranges::all_of(downloads, [](const auto& item) { return item.icon.empty(); });
    if (!gCupertino) {
      glyph(onlyDownloads ? "download" : "stack-2", 22, 18, 22, islandTint(kAppleBlue, ColorRole::Primary));
      label(
          i18n::tr(onlyDownloads ? "island.downloads.title" : "island.downloads.in-progress"), 56, 17, w - 78,
          Style::fontSizeTitle, foreground, false, 1, FontWeight::SemiBold
      );
    }
    const auto rows = downloadRows;
    for (std::size_t i = 0; i < rows; ++i) {
      const float y = (gCupertino ? 18.0F : 57.0F) + static_cast<float>(i) * 55;
      sectionCard(y - 6, y + 45);
      const auto& download = downloads[i];
      // Cupertino leads each row with a round badge; paused transfers use amber.
      const float textX = gCupertino ? 62 : 22;
      if (gCupertino)
        leadingBadge(
            island::transferGlyph(download), 22, y + 4, 30, download.paused() ? kAppleOrange : kAppleBlue,
            ColorRole::Primary
        );
      const float titleEnd = download.determinate ? 88 : 22;
      label(download.name, textX, y, w - textX - titleEnd, 13, foreground, false, 1, FontWeight::Normal, true);
      if (download.paused()) {
        label(i18n::tr("island.downloads.paused"), textX, y + 24, w - textX - 22, 12, islandFixed(kAppleOrange, 1.0F));
        if (download.determinate) {
          auto* percentage =
              label(std::format("{}%", std::lround(download.progress * 100)), w - 76, y, 54, 13, muted, true);
          percentage->setTextAlign(TextAlign::End);
          inst.downloadUi.push_back({download.key, percentage, nullptr});
        }
      } else if (download.determinate) {
        auto* percentage =
            label(std::format("{}%", std::lround(download.progress * 100)), w - 76, y, 54, 13, muted, true);
        percentage->setTextAlign(TextAlign::End);
        auto* bar = progress(static_cast<float>(download.progress), textX, y + 26, w - textX - 22, 7);
        inst.downloadUi.push_back({download.key, percentage, bar});
      } else {
        label(i18n::tr("island.downloads." + download.phase), textX, y + 24, w - textX - 22, 12, muted);
      }
      if (downloadActions[i]) {
        auto* button =
            control(12, y - 4, w - 24, 49, "", "", download.name, 0, true, [this, &inst, key = download.key] {
              activateTransfer(inst, key);
            });
        button->setRadius(Style::scaledRadiusXl(s));
        setIslandActionStyle(button);
        button->inputArea()->setTabFocusKey("download:" + download.key);
      }
    }
    h = (gCupertino ? 22.0F : 60.0F) + static_cast<float>(rows) * 55;
    if (downloads.size() > rows) {
      label(i18n::trp("island.downloads.more", downloads.size() - rows), 22, h, w - 44, 12, muted);
      h += 28;
    }
    if (player && cfg.hoverShowMedia && !showSwitcher && !contextualHeader) {
      pill(control(22, h, w - 44, 32, i18n::tr("island.downloads.media"), "", "", 0, true, [panel] {
        panel("media");
      }));
      h += 38;
    }

  } else if (view == island::View::Rest || view == island::View::Activity || view == island::View::Calendar) {
    float size = view == island::View::Calendar ? cfg.clockSize * 1.4F : cfg.clockSize;
    const bool announce = view == island::View::Activity && !announcement.empty();
    const float inset = (view == island::View::Activity ? (showBattery && showUnread ? 96.0F : 65.0F)
                             : view == island::View::Rest && showBattery
                             ? (showUnread ? 88.0F : 56.0F)
                             : (view == island::View::Rest && showUnread ? badgeWidth + 12.0F : 12.0F))
        + (compactView ? privacyWidth : 0);
    if (view == island::View::Rest && (showUnread || showBattery)) {
      // Symmetric space keeps the clock centred, including large-font settings.
      const auto metrics = renderer.measureText(
          time, size * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
      );
      const float available = (w - inset * 2) * s;
      if (metrics.width > available)
        size *= available / metrics.width;
    }
    Label* clockLabel = nullptr;
    if (view != island::View::Calendar || cfg.hoverShowClock) {
      if (announce) {
        const float textScale = std::min(1.0F, (cfg.height - 8.0F) / 42.0F);
        auto* title = label(
            announcement, inset, 0, w - inset * 2, 17 * textScale, foreground, true, 1, FontWeight::SemiBold, true
        );
        auto* artistLabel = announcementArtist.empty()
            ? nullptr
            : label(
                  announcementArtist, inset, 0, w - inset * 2, 12 * textScale, muted, true, 1, FontWeight::Normal, true
              );
        const float gap = artistLabel ? 2 * textScale * s : 0;
        const float textHeight = title->height() + gap + (artistLabel ? artistLabel->height() : 0);
        const float y = std::max(0.0F, (cfg.height * s - textHeight) / 2);
        title->setPosition(inset * s, y);
        if (artistLabel)
          artistLabel->setPosition(inset * s, y + title->height() + gap);
      } else if (!gCupertino || view != island::View::Activity) {
        clockLabel = label(time, inset, 0, w - inset * 2, size, foreground, true);
        const float clockY = (cfg.height * s - clockLabel->height()) / 2.0F + cfg.clockOffset * s;
        clockLabel->setPosition(
            inset * s, std::clamp(clockY, 0.0F, std::max(0.0F, cfg.height * s - clockLabel->height()))
        );
      }
      action(0, 0, w, cfg.height, "controls", [panel] { panel("control-center"); });
    }
    if (view == island::View::Activity) {
      artwork(12, (cfg.height - 38) / 2, 38);
      const float mediaStatusX =
          w - 44 - (gCupertino ? privacyWidth + (showBattery ? 42 : 0) + (showUnread ? 36 : 0) : 0);
      if (showMediaStatus && !playing)
        glyph("media-pause", mediaStatusX, (cfg.height - 24) / 2, 24, muted);
      if (showVisualizer) {
        if (!retainedVisualizer)
          retainedVisualizer = std::make_unique<IslandAudioVisualizer>(m_spectrum, *inst.surface);
        inst.visualizer = static_cast<IslandAudioVisualizer*>(retainedVisualizer.get());
        // Foreground, not the accent: white on the black capsule, like Apple's Now Playing waveform.
        inst.visualizer->setGradient(foreground, foreground);
        inst.visualizer->setPosition(mediaStatusX * s, (cfg.height - 24) * s / 2);
        inst.visualizer->setSize(24 * s, 24 * s);
        canvas->addChild(std::move(retainedVisualizer));
      }
    }
    if (view == island::View::Calendar) {
      h = (cfg.hoverShowClock ? cfg.height : 0) + (cfg.hoverShowCalendar ? 72 : 8);
      // Keep the clock above a grouped week strip with a clear current-day selection.
      if (clockLabel && cfg.hoverShowCalendar) {
        const float expandedY = ((cfg.height + 19.0F) * s - clockLabel->height()) / 2.0F + cfg.expandedClockOffset * s;
        clockLabel->setPosition(
            inset * s, std::clamp(expandedY, 0.0F, std::max(0.0F, (cfg.height + 6.0F) * s - clockLabel->height()))
        );
      }
      if (cfg.hoverShowCalendar) {
        constexpr float dateSize = 17.0F;
        constexpr float daySize = 13.0F;
        const float preferredCellWidth = cfg.calendarLabels == IslandCalendarLabels::Initials ? 32.0F : 44.0F;
        const float cellWidth = std::min(preferredCellWidth, (w - 40.0F) / 7.0F);
        constexpr float dayHeight = daySize * 1.4F;
        const float stripX = (w - 7.0F * cellWidth) / 2.0F;
        const float calendarTop = cfg.hoverShowClock ? cfg.height : 0;
        const float stripY = calendarTop + 10.0F;
        const auto now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        tm.tm_mday -= 3;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        std::mktime(&tm);
        for (int day = 0; day < 7; ++day) {
          // Orbit's strip fans out from today: each step away is smaller and more transparent.
          const bool today = day == 3;
          const int distance = std::abs(day - 3);
          const float grade = today ? 1.0F : 1.0F - static_cast<float>(distance - 1) * 0.07F;
          auto color = today ? colorSpecFromRole(ColorRole::Primary) : islandRole(ColorRole::OnSurfaceVariant);
          color.alpha *= std::max(0.2F, 1.0F - static_cast<float>(distance) * 0.27F);
          char dayName[64]{};
          std::strftime(dayName, sizeof(dayName), "%a", &tm);
          const bool abbreviated = cfg.calendarLabels == IslandCalendarLabels::Abbreviated
              || (today && cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
          char* shortName = g_utf8_substring(dayName, 0, abbreviated ? 3 : 1);
          const float x = stripX + static_cast<float>(day) * cellWidth;
          auto* weekday = label(
              shortName, x, stripY, cellWidth, daySize, color, true, 1, today ? FontWeight::Bold : FontWeight::SemiBold
          );
          weekday->setPosition(x * s, stripY * s + (dayHeight * s - weekday->height()) / 2.0F);
          g_free(shortName);
          label(
              std::to_string(tm.tm_mday), x, stripY + dayHeight + 2.0F, cellWidth,
              std::round(dateSize * grade * (today ? 1.25F : 1.0F)), color, true, 1,
              today ? FontWeight::Bold : FontWeight::Medium
          );
          ++tm.tm_mday;
          tm.tm_isdst = -1;
          std::mktime(&tm);
        }
        action(0, calendarTop, w, h - calendarTop, "calendar", [panel] { panel("calendar"); });
      }
    }
  } else if (view == island::View::RecordingActivity) {
    glyph("player-record-filled", 22, (cfg.height - 24) / 2, 24, islandTint(kAppleRed, ColorRole::Error));
    const float inset = 70.0F + privacyWidth;
    const float fontScale = std::min(1.0F, std::max(0.0F, cfg.height - 8) / 44);
    auto* title = label(i18n::tr("island.capture.recording"), inset, 0, w - inset * 2, 11 * fontScale, muted, true);
    inst.recordingLabel =
        label(recordingTime, inset, 0, w - inset * 2, 22 * fontScale, foreground, true, 1, FontWeight::SemiBold);
    const float top = (cfg.height * s - title->height() - inst.recordingLabel->height() - 2 * s) / 2;
    title->setPosition(title->x(), top);
    inst.recordingLabel->setPosition(inst.recordingLabel->x(), top + title->height() + 2 * s);
    action(0, 0, w, cfg.height, "capture-open", openCapture);
  } else if (view == island::View::Capture || cameraView) {
    const bool onlyRecording = !cameraView && recording && m_screenSessions.sessions().empty();
    if (gCupertino && onlyRecording) {
      h = 12;
    } else {
      const auto tint = cameraView ? kAppleGreen : onlyRecording ? kAppleRed : kApplePurple;
      leadingBadge(
          cameraView          ? "privacy-camera"
              : onlyRecording ? "player-record-filled"
                              : "privacy-screen",
          22, 20, 40, tint, ColorRole::Primary
      );
      const auto title = cameraView ? "bar.widgets.privacy.camera"
          : onlyRecording           ? "island.capture.recording"
          : recording               ? "island.capture.title"
                                    : "bar.widgets.privacy.screen-sharing";
      if (!gCupertino)
        label(i18n::tr(title), 74, 17, w - 130, 17, foreground, false, 1, FontWeight::SemiBold);
      label(
          i18n::tr(cameraView ? "island.camera.detail" : "island.capture.detail"), 74, gCupertino ? 29 : 43, w - 130,
          12, muted
      );
      auto* settings =
          control(w - 50, 18, 28, 28, "", "settings", i18n::tr("island.capture.settings"), 16, true, [panel] {
            panel("privacy");
          });
      settings->inputArea()->setAcceptedButtons(0);
      action(w - 50, 18, 28, 28, "capture-privacy", [panel] { panel("privacy"); });
    }
  } else if (view == island::View::AwakeActivity && awake) {
    glyph("caffeine-on", 22, (cfg.height - 24) / 2, 24, islandTint(kAppleOrange, ColorRole::Primary));
    const float inset = (showUnread ? 95.0F : 70.0F) + privacyWidth;
    const float available = std::max(1.0F, w - 2 * inset);
    const float fontScale = std::min(1.0F, std::max(0.0F, cfg.height - 8) / 44);
    auto* title = label(i18n::tr("utilities.keep-awake.title"), inset, 0, available, 11 * fontScale, muted, true);
    inst.awakeLabel =
        label(activityTime(*awake), inset, 0, available, 22 * fontScale, foreground, true, 1, FontWeight::SemiBold);
    const float top = (cfg.height * s - title->height() - inst.awakeLabel->height() - 2 * s) / 2;
    title->setPosition(title->x(), top);
    inst.awakeLabel->setPosition(inst.awakeLabel->x(), top + title->height() + 2 * s);
  } else if (view == island::View::Awake && awake) {
    leadingBadge("caffeine-on", 22, 18, 40, kAppleOrange, ColorRole::Primary);
    if (!gCupertino)
      label(i18n::tr("utilities.keep-awake.title"), 74, 16, w - 130, 17, foreground, false, 1, FontWeight::SemiBold);
    label(i18n::tr("island.awake.detail"), 74, gCupertino ? 29 : 42, w - 130, 12, muted);
    auto* power = control(w - 50, 18, 28, 28, "", "settings", i18n::tr("island.awake.settings"), 16, true, [panel] {
      panel("power");
    });
    power->inputArea()->setAcceptedButtons(0);
    action(w - 50, 18, 28, 28, "awake-power", [panel] { panel("power"); });
    inst.awakeLabel = label(activityTime(*awake), 22, 68, w - 44, 34, foreground, true, 1, FontWeight::SemiBold);
    label(i18n::tr("island.awake.remaining"), 22, 112, w - 44, 11, muted, true);
    const float buttonWidth = (w - 52) / 2;
    auto* extend = control(
        22, 140, buttonWidth, 32, i18n::tr("island.awake.extend"), "", "", 0, true,
        [this] {
          if (m_idle)
            m_idle->extendTimed(15min);
        },
        13
    );
    extend->inputArea()->setTabFocusKey("awake-extend");
    pill(extend);
    auto* end = control(
        30 + buttonWidth, 140, buttonWidth, 32, i18n::tr("island.awake.end"), "", "", 0, true,
        [this] {
          if (awakeRemaining())
            m_idle->setEnabled(false);
        },
        13
    );
    end->inputArea()->setTabFocusKey("awake-end");
    pill(end);
  } else if (view == island::View::Microphone && microphone) {
    leadingBadge("microphone", 22, 20, 40, kAppleOrange, ColorRole::Primary);
    if (gCupertino)
      label(microphone->appNames(), 74, 29, w - 130, 17, foreground, false, 1, FontWeight::SemiBold, true);
    else {
      label(i18n::tr("island.microphone.title"), 74, 17, w - 130, 17, foreground, false, 1, FontWeight::SemiBold);
      label(microphone->appNames(), 74, 43, w - 96, 12, muted, false, 1, FontWeight::Normal, true);
    }
    auto* audio =
        control(w - 50, 18, 28, 28, "", "settings", i18n::tr("island.privacy.audio-controls"), 16, true, [panel] {
          panel("audio");
        });
    audio->inputArea()->setAcceptedButtons(0);
    action(w - 50, 18, 28, 28, "microphone-audio", [panel] { panel("audio"); });
  } else if (view == island::View::Media && player) {
    const float artworkSize =
        gCupertino ? std::max(cfg.mediaArtworkSize, cfg.compactLayout ? 50.0F : 64.0F) : cfg.mediaArtworkSize;
    const float mediaOffset = std::max(0.0F, artworkSize - 56.0F);
    const float textX = 27.0F + artworkSize + 16.0F;
    const float waveformWidth = showVisualizer && gCupertino ? 54.0F : 0;
    artwork(27, 22, artworkSize);
    label(
        player->title, textX, 27 + mediaOffset / 2.0F, w - textX - 25 - waveformWidth,
        gCupertino ? (cfg.compactLayout ? 16 : 19) : Style::fontSizeTitle, foreground, false, 1, FontWeight::SemiBold,
        true
    );
    label(
        joinedArtists(player->artists), textX, 53 + mediaOffset / 2.0F, w - textX - 25 - waveformWidth,
        gCupertino ? 14 : Style::fontSizeCaption, muted
    );
    const auto& source = player->identity.empty() ? player->desktopEntry : player->identity;
    if (!gCupertino && !source.empty()) {
      label(source, textX, 73 + mediaOffset / 2.0F, w - textX - 42, Style::fontSizeMini, muted);
      glyph("chevron-right", w - 37, 73 + mediaOffset / 2.0F, 12, muted);
    }
    if (showVisualizer && gCupertino) {
      if (!retainedVisualizer)
        retainedVisualizer = std::make_unique<IslandAudioVisualizer>(m_spectrum, *inst.surface);
      inst.visualizer = static_cast<IslandAudioVisualizer*>(retainedVisualizer.get());
      const auto accent = m_flow.accent();
      inst.visualizer->setGradient(islandFixed(rgba(accent.r, accent.g, accent.b), 1), foreground);
      inst.visualizer->setPosition((w - 70) * s, (22 + (artworkSize - 30) / 2) * s);
      inst.visualizer->setSize(40 * s, 30 * s);
      canvas->addChild(std::move(retainedVisualizer));
    }
    const float fraction =
        player->lengthUs > 0 ? static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs) : 0;
    inst.seekProgress =
        progress(inst.seeking ? inst.seekFraction : fraction, 27, 104 + mediaOffset, w - 54, gCupertino ? 4.0F : 8.0F);
    if (gCupertino && cfg.mediaGradient && m_flowArt == artPath && m_flow.hasArtwork()) {
      const auto accent = m_flow.accent();
      inst.seekProgress->setFill(islandFixed(rgba(accent.r, accent.g, accent.b), 1.0F));
    }
    inst.mediaPosition = label(
        std::format("{}:{:02}", displayPosition / 60, displayPosition % 60), 27, 118 + mediaOffset, 65,
        Style::fontSizeMini, inst.seeking ? islandRole(ColorRole::Primary) : muted
    );
    const auto seconds = player->lengthUs / 1000000;
    label(
        std::format("{}:{:02}", seconds / 60, seconds % 60), w - 73, 118 + mediaOffset, 46, Style::fontSizeMini, muted,
        true
    );
    const std::string bus = player->busName;
    const auto button = [&](float x, const std::string& icon, const std::string& tooltip, bool available,
                            std::function<void()> cb) {
      auto* mediaControl =
          control(x, 137 + mediaOffset, 44, 48, "", icon, tooltip, gCupertino ? 26 : 23, available, std::move(cb));
      if (icon == "media-play" || icon == "media-pause") {
        mediaControl->inputArea()->setTabFocusKey("playback");
        setIslandVariant(mediaControl, gCupertino ? ButtonVariant::Ghost : ButtonVariant::Default);
        mediaControl->setRadius(Style::scaledRadius(22, s));
      }
    };
    const float spread = gCupertino && !cfg.compactLayout ? 82.0F : 67.0F;
    button(
        w / 2 - 22 - spread, "media-prev", i18n::tr("control-center.media.previous"), player->canGoPrevious,
        [this, bus] { m_mpris->previous(bus); }
    );
    button(
        w / 2 - 22, playing ? "media-pause" : "media-play",
        i18n::tr(playing ? "control-center.media.pause" : "control-center.media.play"),
        playing ? player->canPause : player->canPlay, [this, bus] { m_mpris->playPause(bus); }
    );
    button(w / 2 - 22 + spread, "media-next", i18n::tr("control-center.media.next"), player->canGoNext, [this, bus] {
      m_mpris->next(bus);
    });
    action(20, 15, w - 40, 81 + mediaOffset, "media-panel", [panel] { panel("media"); });
    if (player->canSeek && player->lengthUs > 0) {
      inst.seekLengthUs = player->lengthUs;
      inst.seekX = 27;
      inst.seekY = 96 + mediaOffset;
      inst.seekWidth = w - 54;
      inst.seek = [this, bus, length = player->lengthUs](float seekFraction) {
        m_mpris->setPosition(bus, static_cast<std::int64_t>(static_cast<double>(length) * seekFraction));
      };
    }
    h = 190 + mediaOffset;
  } else if (view == island::View::Connection) {
    const auto& connection = *inst.connection;
    const auto status =
        i18n::tr(connection.audioOutput ? "island.connection.audio-output" : "island.connection.connected");
    const auto detail = connection.percentage
        ? i18n::tr(
              "island.connection.battery-detail", "status", status, "percentage", std::lround(*connection.percentage)
          )
        : status;
    const float x = statusText(connection.name, detail);
    glyph(connection.icon, x, (h - statusIconSize) / 2, statusIconSize, foreground);
    if (!connection.panel.empty() && openPanel) {
      const auto tooltip = connection.name
          + "\n"
          + i18n::tr(connection.panel == "audio" ? "island.connection.open-audio" : "island.connection.open-bluetooth");
      auto* button = control(
          6, 6, w - 12, h - 12, "", "", tooltip, 0, true,
          [this, &inst, panel, key = connection.actionKey(), destination = connection.panel] {
            if (!inst.connection
                || inst.connection->actionKey() != key
                || island::BatteryConnections::Clock::now()
                    >= inst.connection->started + std::chrono::seconds(inst.config.bluetoothPreviewSeconds))
              return;
            m_batteryConnections->dismissPreview();
            panel(destination);
          }
      );
      button->setRadius((h - 12) * s / 2);
      setIslandActionStyle(button);
      button->inputArea()->setTabFocusKey(connection.actionKey());
    } else {
      auto area = std::make_unique<InputArea>();
      area->setSize(w * s, h * s);
      area->setAcceptedButtons(0);
      area->setTooltip(connection.name);
      canvas->addChild(std::move(area));
    }
  } else if (view == island::View::Network) {
    const auto notice = *inst.network;
    const float x = statusText(notice.title(), notice.detail());
    glyph(
        notice.icon(), x, (h - statusIconSize) / 2, statusIconSize,
        islandFixed(notice.kind == island::NetworkNotice::Kind::Lost ? kAppleOrange : kAppleGreen, 1.0F)
    );
    const auto tooltip = notice.title()
        + (notice.detail().empty() ? "" : "\n" + notice.detail())
        + "\n"
        + i18n::tr("island.network.open-controls");
    auto* button = control(
        6, 6, w - 12, h - 12, "", "", tooltip, 0, static_cast<bool>(openPanel),
        [this, &inst, panel, serial = notice.serial] {
          if (!inst.network
              || inst.network->serial != serial
              || island::NetworkNotice::Clock::now()
                  >= inst.network->started + std::chrono::seconds(inst.config.networkPreviewSeconds))
            return;
          m_networkActivity.dismiss(serial);
          panel("network");
        }
    );
    button->setRadius((h - 12) * s / 2);
    setIslandActionStyle(button);
    button->inputArea()->setTabFocusKey(notice.actionKey());
  } else if (view == island::View::TransferNotice) {
    const bool failed = m_transferNotice->notice == island::TransferNotice::Failed;
    const auto message = i18n::tr(
        failed ? "island.downloads.failed"
            : m_transferNotice->notice == island::TransferNotice::TransferFinished
            ? "island.downloads.transfer-finished"
            : "island.downloads.finished"
    );
    const auto& title = m_transferNotice->title;
    const auto& detail = m_transferNotice->detail;
    const float x = statusText(
        title.empty() ? message : title,
        !detail.empty()     ? detail
            : title.empty() ? ""
                            : message
    );
    auto tooltip = m_transferNotice->source.name;
    if (!title.empty())
      tooltip += (tooltip.empty() ? "" : "\n") + title;
    if (!detail.empty())
      tooltip += (tooltip.empty() ? "" : "\n") + detail;
    glyph(
        failed ? "circle-x-filled" : "circle-check-filled", x, (h - statusIconSize) / 2, statusIconSize,
        islandFixed(failed ? kAppleRed : kAppleGreen, 1.0F)
    );
    if (transferAction) {
      auto* button =
          control(6, 6, w - 12, h - 12, "", "", tooltip, 0, true, [this, &inst, serial = m_transferNotice->serial] {
            activateTransferNotice(inst, serial);
          });
      button->setRadius((h - 12) * s / 2);
      setIslandActionStyle(button);
      button->inputArea()->setTabFocusKey("transfer:" + std::to_string(m_transferNotice->serial));
    } else if (!tooltip.empty()) {
      auto area = std::make_unique<InputArea>();
      area->setSize(w * s, h * s);
      area->setAcceptedButtons(0);
      area->setTooltip(tooltip);
      canvas->addChild(std::move(area));
    }
  } else if (view == island::View::Osd && m_osd && (m_osd->kind == OsdKind::Dnd || m_osd->kind == OsdKind::Charging)) {
    // Focus and charging keep their coloured badges beside the shared two-line status layout.
    const bool charging = m_osd->kind == OsdKind::Charging;
    const bool on = charging || !m_osd->inactive;
    const Color tint = charging ? kAppleGreen : kAppleIndigo;
    const ColorRole role = charging ? ColorRole::Secondary : ColorRole::Primary;
    constexpr float badgeSize = statusIconSize;
    const float badgeX = statusText(
        i18n::tr(charging ? "island.status.charging" : "island.status.dnd"),
        charging ? std::format("{}%", std::lround(m_osd->progress * 100))
                 : i18n::tr(on ? "island.status.on" : "island.status.off")
    );
    const std::string icon = charging ? "bolt-filled" : on ? "focus-on" : "focus-off";
    if (gCupertino) {
      // Big Sur's Do Not Disturb tile: a solid round toggle, the name, and the state under it.
      auto disc = std::make_unique<Box>();
      disc->setFill(on ? islandFixed(tint, 1.0F) : islandRole(ColorRole::OnSurface, 0.18F));
      disc->setRadius(badgeSize * s / 2);
      disc->setSize(badgeSize * s, badgeSize * s);
      disc->setPosition(badgeX * s, (h - badgeSize) / 2 * s);
      disc->setHitTestVisible(false);
      canvas->addChild(std::move(disc));
      constexpr float symbolSize = badgeSize / 2;
      glyph(
          icon, badgeX + (badgeSize - symbolSize) / 2, (h - symbolSize) / 2, symbolSize,
          on ? islandFixed(rgba(1.0F, 1.0F, 1.0F), 1.0F) : muted
      );
    } else {
      if (on)
        leadingBadge(icon, badgeX, (h - badgeSize) / 2, badgeSize, tint, role);
      else
        leadingBadge(icon, badgeX, (h - badgeSize) / 2, badgeSize, rgba(1.0F, 1.0F, 1.0F), ColorRole::OnSurfaceVariant);
    }
  } else if (view == island::View::Osd && m_osd && (m_osd->kind == OsdKind::LockKeys || !m_osd->showProgress)) {
    // Status messages without a level centre their icon and text as one group.
    constexpr float iconSize = 26.0F;
    constexpr float gap = 12.0F;
    const float maxTextWidth = std::max(1.0F, w - 40 - iconSize - gap);
    const auto metrics = renderer.measureText(
        m_osd->value, 15 * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
    );
    const float textWidth = std::min(maxTextWidth, metrics.width / s);
    const float x = (w - iconSize - gap - textWidth) / 2;
    glyph(m_osd->icon, x, (h - iconSize) / 2, iconSize);
    auto* value = label(m_osd->value, x + iconSize + gap, 0, textWidth, 15);
    value->setPosition(value->x(), (h * s - value->height()) / 2);
  } else if (
      view == island::View::Osd
      && m_osd
      && gCupertino
      && (m_osd->kind == OsdKind::Volume || m_osd->kind == OsdKind::Microphone || m_osd->kind == OsdKind::Brightness)
  ) {
    // Big Sur's Sound and Display modules: a white-filled groove with the symbol inside its leading
    // end. Display and Microphone name themselves above it; Sound is just a larger bar, centred.
    const bool volume = m_osd->kind == OsdKind::Volume;
    const float trackHeight = volume ? std::clamp(cfg.volumeBarHeight + 10.0F, 14.0F, 34.0F) : 18.0F;
    const bool symbolInside = trackHeight >= 14.0F;
    constexpr float margin = 20.0F;
    const float trackX = symbolInside ? margin : margin + 32.0F;
    float trackRight = w - margin;
    float trackY = 0.0F;
    if (volume) {
      trackY = (h - trackHeight) / 2;
      if (cfg.volumeShowPercentage) {
        const auto metrics = renderer.measureText(
            m_osd->value, 13 * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
        );
        const float valueWidth = std::ceil(metrics.width / s) + 2;
        auto* valueLabel = label(m_osd->value, w - margin - valueWidth, 0, valueWidth, 13, muted);
        valueLabel->setPosition(valueLabel->x(), (h * s - valueLabel->height()) / 2);
        trackRight -= valueWidth + 10;
      }
    } else {
      const std::string title =
          i18n::tr(m_osd->kind == OsdKind::Microphone ? "island.osd.microphone" : "island.osd.display");
      auto* titleLabel = label(title, margin, 9, w - 2 * margin - 64, 13, foreground, false, 1, FontWeight::SemiBold);
      const auto metrics = renderer.measureText(
          m_osd->value, 13 * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
      );
      const float valueWidth = std::ceil(metrics.width / s) + 2;
      auto* valueLabel = label(m_osd->value, w - margin - valueWidth, 9, valueWidth, 13, muted);
      valueLabel->setPosition(valueLabel->x(), titleLabel->y());
      const float top = 9 + titleLabel->height() / s + 6;
      trackY = top + std::max(0.0F, (h - top - 10 - trackHeight) / 2);
    }
    const float trackWidth = std::max(trackHeight, trackRight - trackX);
    const auto groove = [&](float x, float width, ColorSpec fill) {
      auto node = std::make_unique<Box>();
      node->setFill(fill);
      node->setRadius(trackHeight * s / 2);
      node->setSize(width * s, trackHeight * s);
      node->setPosition(x * s, trackY * s);
      node->setHitTestVisible(false);
      canvas->addChild(std::move(node));
    };
    groove(trackX, trackWidth, islandRole(ColorRole::OnSurface, 0.2F));
    // The fill starts as a round end, so the symbol always sits on white; muted reads grey.
    const float level = std::clamp(m_osd->progress, 0.0F, 1.0F);
    groove(
        trackX, trackHeight + level * (trackWidth - trackHeight),
        m_osd->inactive ? islandRole(ColorRole::OnSurface, 0.55F) : islandFixed(rgba(1.0F, 1.0F, 1.0F), 1.0F)
    );
    if (symbolInside) {
      const float symbolSize = std::round(trackHeight * 0.62F);
      glyph(
          m_osd->icon, trackX + (trackHeight - symbolSize) / 2, trackY + (trackHeight - symbolSize) / 2, symbolSize,
          islandFixed(rgba(0.22F, 0.22F, 0.24F), 1.0F)
      );
    } else {
      glyph(m_osd->icon, margin, trackY + trackHeight / 2 - 11, 22);
    }
  } else if (view == island::View::Osd && m_osd) {
    // Icon and level span the Island with equal margins, so the group sits centred.
    glyph(m_osd->icon, 20, (h - 26) / 2, 26);
    if (m_osd->kind == OsdKind::Volume) {
      const float barHeight = cfg.volumeBarHeight;
      if (cfg.volumeShowPercentage)
        label(m_osd->value, 60, 12, w - 80, 15);
      const float barCenter = cfg.volumeShowPercentage ? 44.0F : h / 2.0F;
      progress(m_osd->progress, 60, barCenter - barHeight / 2.0F, w - 80, barHeight);
    } else {
      label(m_osd->value, 60, 12, w - 80, 15);
      progress(m_osd->progress, 60, 43, w - 80);
    }
  } else if (view == island::View::Notification && m_notification) {
    const auto n = *m_notification;
    const bool expanded = inst.expandedNotification == n.id;
    // The sending app's icon leads the banner at full size, as on a macOS banner, with the app
    // name, title and body in a column beside it; the text starts at the edge when none resolves.
    // The same size as the unread card in the expanded view, so the icon doesn't jump between them.
    const float appIconSize = 32.0F;
    const bool recordingResult =
        n.origin == NotificationOrigin::Internal && n.category == kRecordingNotificationCategory;
    // Capture images are attachments, separate from the sender's icon.
    const bool thumbnail = (n.category == kScreenshotNotificationCategory || recordingResult)
        && n.imageData
        && n.imageData->width > 0
        && n.imageData->height > 0
        && n.imageData->channels == 4
        && n.imageData->data.size() >= static_cast<std::size_t>(n.imageData->rowStride) * n.imageData->height;
    Notification iconSource = n;
    if (thumbnail)
      iconSource.imageData.reset();
    const bool hasAppIcon = notificationIcon(iconSource, 24, 20, appIconSize);
    const float textX = hasAppIcon ? 24 + appIconSize + 12 : 22;
    const float appLabelX = textX;
    // "now" / "5m ago" closes the header row, as on the notification banners.
    constexpr float timeWidth = 56.0F;
    auto* appLabel = label(n.appName, appLabelX, 14, w - 55 - timeWidth - appLabelX, Style::fontSizeCaption, muted);
    auto* timeLabel = label(
        formatNotificationTime(n.receivedWallClock.value_or(WallClock::now())), w - 51 - timeWidth, 14, timeWidth,
        Style::fontSizeCaption, muted
    );
    timeLabel->setTextAlign(TextAlign::End);
    timeLabel->measure(renderer);
    control(w - 47, 5, 32, 30, "", "x", i18n::tr("notifications.dismiss"), 18, true, [this] { dismissNotification(); });
    std::vector<std::pair<std::string, std::string>> visibleActions;
    const bool hasDefault = std::ranges::find(n.actions, "default") != n.actions.end();
    if ((recordingResult || expanded || inst.keyboardMode) && hasDefault)
      visibleActions.emplace_back(
          "default", i18n::tr(recordingResult ? "notifications.internal.recording-play" : "notifications.actions.open")
      );
    for (std::size_t index = 0; index + 1 < n.actions.size() && visibleActions.size() < 3; index += 2)
      if (n.actions[index] != "default")
        visibleActions.emplace_back(n.actions[index], n.actions[index + 1]);
    // macOS keeps actions out of sight: hovering shows the one action, or "Options" for several,
    // in place of the time stamp, and only an opened notification (or keyboard mode) lists them.
    const bool actionsOpen = recordingResult || expanded || inst.keyboardMode;
    const bool hasActions = actionsOpen && !visibleActions.empty();
    const float maxHeight = std::min(
        expanded ? 640.0F : 360.0F,
        inst.outputHeight / s - 16.0F - 2 * cardGutter - (privacyList.empty() ? 0.0F : 32.0F)
    );
    const float footerHeight = hasActions ? 46.0F : 16.0F;
    const float textBottom = maxHeight - footerHeight;
    const bool hasBody = n.body.find_first_not_of(" \t\r\n") != std::string::npos;
    // The thumbnail sits at the card's right, like an attachment on a macOS notification.
    constexpr float thumbnailHeight = 64.0F;
    const float thumbnailWidth = recordingResult ? 114.0F
        : thumbnail
        ? std::min(
              120.0F, thumbnailHeight * static_cast<float>(n.imageData->width) / static_cast<float>(n.imageData->height)
          )
        : 0.0F;
    const float textWidth =
        w - (expanded ? 64.0F : 44.0F) - (textX - 22.0F) - (thumbnailWidth > 0 ? thumbnailWidth + 12.0F : 0.0F);
    auto* summary =
        label(n.summary, textX, 37, textWidth, Style::fontSizeTitle, foreground, false, 0, FontWeight::SemiBold);
    const float fullSummaryHeight = summary->height();
    // Pango's line limit is per paragraph. Flatten hard breaks only in the
    // compact preview so many short paragraphs cannot exceed its height cap.
    const auto previewText = [](std::string text) {
      std::replace(text.begin(), text.end(), '\n', ' ');
      std::replace(text.begin(), text.end(), '\r', ' ');
      return text;
    };
    bool truncated = false;
    if (!expanded) {
      summary->setText(previewText(n.summary));
      const int summaryLines = hasBody ? 4 : 12;
      summary->setMaxLines(summaryLines);
      summary->measure(renderer);
      const float summaryHeight = std::max(0.0F, textBottom - 37.0F - (hasBody ? 32.0F : 0.0F)) * s;
      for (int lines = summaryLines - 1; summary->height() > summaryHeight && lines >= 1; --lines) {
        summary->setMaxLines(lines);
        summary->measure(renderer);
      }
      truncated = fullSummaryHeight > summary->height() + 0.5F;
    }
    float contentBottom = 37.0F + summary->height() / s;
    Label* body = nullptr;
    if (hasBody) {
      const float bodyY = contentBottom + 8.0F;
      body = label(n.body, textX, bodyY, textWidth, 13, muted, false, 0);
      const float fullBodyHeight = body->height();
      if (!expanded) {
        body->setText(previewText(n.body));
        const float bodyHeight = std::max(0.0F, textBottom - bodyY) * s;
        body->setMaxLines(12);
        body->measure(renderer);
        for (int lines = 11; body->height() > bodyHeight && lines >= 1; --lines) {
          body->setMaxLines(lines);
          body->measure(renderer);
        }
        body->setVisible(body->height() <= bodyHeight);
        truncated |= !body->visible() || fullBodyHeight > body->height() + 0.5F;
      }
      if (body->visible())
        contentBottom = bodyY + body->height() / s;
    }
    if (recordingResult) {
      const float x = w - 22 - thumbnailWidth;
      auto placeholder = std::make_unique<Box>();
      placeholder->setFill(islandRole(ColorRole::SurfaceVariant));
      placeholder->setRadius(Style::scaledRadiusMd(s));
      placeholder->setPosition(x * s, 37 * s);
      placeholder->setSize(thumbnailWidth * s, thumbnailHeight * s);
      placeholder->setHitTestVisible(false);
      inst.content->addChild(std::move(placeholder));
      glyph("video", x + (thumbnailWidth - 24) / 2, 57, 24, muted);
      contentBottom = std::max(contentBottom, 37.0F + thumbnailHeight);
    }
    if (thumbnail) {
      const auto& raw = *n.imageData;
      auto image = std::make_unique<Image>();
      image->setSize(thumbnailWidth * s, thumbnailHeight * s);
      image->setRadius(Style::scaledRadiusMd(s));
      image->setFit(ImageFit::Cover);
      image->setPosition((w - 22 - thumbnailWidth) * s, 37 * s);
      if (image->setSourceRaw(
              renderer, raw.data.data(), raw.data.size(), raw.width, raw.height, raw.rowStride, PixmapFormat::RGBA, true
          )) {
        inst.content->addChild(std::move(image));
        contentBottom = std::max(contentBottom, 37.0F + thumbnailHeight);
      }
    }
    if (expanded) {
      auto scroll = std::make_unique<ScrollView>();
      scroll->setContentScale(s);
      scroll->setViewportPaddingH(0);
      scroll->setViewportPaddingV(0);
      scroll->content()->setGap(8 * s);
      scroll->content()->addChild(inst.content->removeChild(summary));
      if (body)
        scroll->content()->addChild(inst.content->removeChild(body));
      const float viewportHeight = std::min(contentBottom - 37.0F, std::max(1.0F, textBottom - 37.0F));
      auto* scrollView = scroll.get();
      inst.content->addChild(std::move(scroll));
      scrollView->setSize((thumbnailWidth > 0 ? textWidth : w - 22 - textX) * s, viewportHeight * s);
      scrollView->layout(renderer);
      scrollView->setPosition(textX * s, 37 * s);
      contentBottom = 37.0F + viewportHeight;
    }
    if (hasAppIcon)
      contentBottom = std::max(contentBottom, 20.0F + appIconSize);
    const auto toggleExpanded = [this, &inst, id = n.id] {
      if (inst.expandedNotification == id)
        inst.expandedNotification.reset();
      else
        inst.expandedNotification = id;
      refresh();
    };
    if (expanded || truncated) {
      appLabel->setMinWidth(std::max(0.0F, w - 90 - timeWidth - appLabelX) * s);
      appLabel->setMaxWidth(std::max(0.0F, w - 90 - timeWidth - appLabelX) * s);
      appLabel->measure(renderer);
      timeLabel->setPosition((w - 86 - timeWidth) * s, timeLabel->y());
      auto* expandControl = control(
          w - 82, 5, 32, 30, "", expanded ? "chevron-up" : "chevron-down",
          i18n::tr(expanded ? "notifications.collapse" : "notifications.expand"), 18, true, toggleExpanded
      );
      expandControl->inputArea()->setTabFocusKey("notification-expand");
    }
    // A grey capsule sized to its label, as Apple's notification buttons are.
    const auto pillWidth = [&](const std::string& text) {
      const auto metrics = renderer.measureText(text, Style::fontSizeCaption * s);
      return std::ceil(metrics.width / s) + 24.0F;
    };
    if (!actionsOpen && !visibleActions.empty() && inst.hovered) {
      const bool single = visibleActions.size() == 1;
      const std::string text = single ? visibleActions.front().second : i18n::tr("notifications.actions.options");
      const float width = std::min(pillWidth(text), w / 2.0F);
      const float right = timeLabel->x() / s + timeWidth;
      timeLabel->setVisible(false);
      auto* pillControl = control(
          right - width, 9, width, 24, text, "", text, 0, true,
          [this, single, toggleExpanded, id = n.id, key = visibleActions.front().first] {
            if (single)
              (void)m_notifications->invokeAction(id, key);
            else
              toggleExpanded();
          }
      );
      setIslandVariant(pillControl, ButtonVariant::Default);
      pillControl->setRadius(12.0F * s);
    }
    h = contentBottom + footerHeight;
    const float actionsY = contentBottom + 8.0F;
    float actionX = 22.0F;
    for (std::size_t index = 0; hasActions && index < visibleActions.size(); ++index) {
      const auto& [key, text] = visibleActions[index];
      const float width = std::min(pillWidth(text), w - 22.0F - actionX);
      if (width <= 24.0F)
        break;
      auto* actionControl = control(actionX, actionsY, width, 30, text, "", text, 0, true, [this, id = n.id, key] {
        (void)m_notifications->invokeAction(id, key);
      });
      setIslandVariant(actionControl, ButtonVariant::Default);
      actionControl->setRadius(15.0F * s);
      actionControl->inputArea()->setTabFocusKey("notification-action-" + key);
      actionX += width + 8.0F;
    }
    if (!expanded)
      action(0, 37, w, contentBottom - 37.0F, "notification", [this, n, panel, truncated, toggleExpanded] {
        if (truncated)
          toggleExpanded();
        else if (std::ranges::find(n.actions, "default") != n.actions.end())
          (void)m_notifications->invokeAction(n.id, "default");
        else {
          dismissNotification();
          panel("notifications");
        }
      });
  }
  if (contextualHeader) {
    constexpr float headerHeight = 44;
    for (const auto& child : inst.content->children())
      child->setPosition(child->x(), child->y() + headerHeight * s);
    for (auto& hit : inst.actions)
      hit.y += headerHeight;
    if (inst.seek)
      inst.seekY += headerHeight;
    h += headerHeight;
  }
  if (showSwitcher) {
    // Translate the main card and its manual hit regions together. Tabs remain
    // fixed above both the card and the scrolling footer.
    constexpr float tabHeight = 52;
    for (const auto& child : inst.content->children())
      child->setPosition(child->x(), child->y() + tabHeight * s);
    for (auto& hit : inst.actions)
      hit.y += tabHeight;
    if (inst.seek)
      inst.seekY += tabHeight;
    h += tabHeight;
    sectionCard(12, tabHeight);
    const float tabWidth = (w - 32) / static_cast<float>(availableActivities.count());
    float x = 16;
    for (const auto activity :
         {island::Activity::Media, island::Activity::Downloads, island::Activity::Timers, island::Activity::Microphone,
          island::Activity::Awake, island::Activity::Capture, island::Activity::Camera}) {
      if (!availableActivities.contains(activity))
        continue;
      const std::string key = activity == island::Activity::Media ? "media"
          : activity == island::Activity::Downloads               ? "downloads"
          : activity == island::Activity::Microphone              ? "microphone"
          : activity == island::Activity::Awake                   ? "awake"
          : activity == island::Activity::Capture                 ? "capture"
          : activity == island::Activity::Camera                  ? "camera"
                                                                  : "timers";
      auto* tab = control(
          x, 16, tabWidth - 4, 30, i18n::tr("island.activities." + key), "", "", 0, true, [this, &inst, activity] {
            inst.activities.selected = activity;
            if (inst.config.splitActivities
                && (activity == island::Activity::Media
                    || activity == island::Activity::Downloads
                    || activity == island::Activity::Timers
                    || activity == island::Activity::Awake))
              inst.compactActivity.promote(activity);
            refresh();
          }
      );
      tab->inputArea()->setTabFocusKey("activity-" + key);
      setIslandVariant(tab, activity == inst.activities.selected ? ButtonVariant::TabActive : ButtonVariant::Tab);
      x += tabWidth;
    }
  }
  // Keep the main activity fixed while a busy footer scrolls within the output.
  const float footerTop = h;
  auto footer = std::make_unique<Node>();
  if (expandedView)
    canvas = footer.get();
  if (view == island::View::Downloads && !recentTransfers.empty() && showRecentTransfers) {
    label(i18n::tr("island.downloads.recent"), 22, h + 3, w - 44, 12, muted);
    h += 30;
    for (std::size_t i = 0; i < recentTransfers.size(); ++i) {
      const auto& entry = recentTransfers[i];
      const auto& feedback = entry.feedback;
      const auto message = transferMessage(feedback);
      const auto title = !feedback.title.empty() ? feedback.title
          : !feedback.source.name.empty()        ? feedback.source.name
                                                 : message;
      const bool failed = feedback.notice == island::TransferNotice::Failed;
      sectionCard(h - 6, h + 45);
      glyph(
          failed ? "circle-x-filled" : "circle-check-filled", 24, h + 8, 24,
          islandFixed(failed ? kAppleRed : kAppleGreen, 1.0F)
      );
      label(title, 62, h, w - 146, 13, foreground);
      label(message, 62, h + 22, w - 84, 12, muted);
      auto* age = label(recentTransferAge(entry), w - 76, h, 54, 12, muted);
      age->setTextAlign(TextAlign::End);
      inst.recentTransferUi.emplace_back(feedback.serial, age);
      auto tooltip = title + "\n" + message;
      if (!feedback.source.name.empty() && feedback.source.name != title)
        tooltip = feedback.source.name + "\n" + tooltip;
      if (recentActions[i]) {
        auto* row = control(12, h - 4, w - 24, 49, "", "", tooltip, 0, true, [this, &inst, serial = feedback.serial] {
          activateRecentTransfer(inst, serial);
        });
        row->setRadius(Style::scaledRadiusXl(s));
        setIslandActionStyle(row);
        row->inputArea()->setTabFocusKey("recent-transfer:" + std::to_string(feedback.serial));
      } else {
        auto area = std::make_unique<InputArea>();
        area->setPosition(12 * s, (h - 4) * s);
        area->setSize((w - 24) * s, 49 * s);
        area->setAcceptedButtons(0);
        area->setTooltip(tooltip);
        canvas->addChild(std::move(area));
      }
      h += 55;
    }
    h += 4;
  }
  if (view == island::View::Capture || cameraView) {
    if (recording && !cameraView) {
      if (gCupertino) {
        auto symbol = std::make_unique<Box>();
        symbol->setFill(rgba(0, 0, 0, 0));
        symbol->setBorder(islandFixed(kAppleRed, 1), 3 * s);
        symbol->setSize(42 * s, 42 * s);
        symbol->setRadius(21 * s);
        symbol->setPosition(24 * s, (h + 12) * s);
        canvas->addChild(std::move(symbol));
        glyph("circle-filled", 34, h + 22, 22, islandFixed(kAppleRed, 1));
        inst.recordingLabel = label(
            recordingTime, 86, h + 2, w - 264, cfg.compactLayout ? 28 : 34, foreground, false, 1, FontWeight::SemiBold
        );
        label(i18n::tr("island.capture.recording"), 86, h + 46, w - 264, 13, muted);
      } else {
        label("Noctalia", 22, h + 4, w - 178, 14, foreground, false, 1, FontWeight::SemiBold);
        inst.recordingLabel = label(recordingTime, 22, h + 29, w - 178, 12, islandTint(kAppleRed, ColorRole::Error));
      }
      const auto session = ScreenRecorder::instance().sessionId();
      auto* stop = control(
          w - 148, h + 10, 126, 34, i18n::tr("island.capture.stop"), "", "", 0, !ScreenRecorder::instance().stopping(),
          [this, session] {
            auto& recorder = ScreenRecorder::instance();
            if (recorder.sessionId() == session) {
              recorder.stop();
              refresh();
            }
          },
          12
      );
      stop->inputArea()->setTabFocusKey("capture-stop");
      pill(stop);
      h += gCupertino ? 86 : 66;
    }
    for (const auto& [app, started] : appSessions.sessions()) {
      label(app, 22, h + 4, w - 178, 14, foreground, false, 1, FontWeight::SemiBold, true);
      const auto elapsed = *appSessions.elapsed(app, island::CaptureSessions::Clock::now());
      auto* elapsedLabel = label(
          i18n::tr("island.capture.elapsed", "time", activityTime(elapsed)), 22, h + 29, w - 178, 12,
          islandTint(cameraView ? kAppleGreen : kApplePurple, ColorRole::Primary)
      );
      inst.captureLabels.emplace_back(app, elapsedLabel);
      auto* open = control(
          w - 148, h + 10, 126, 34, i18n::tr("utilities.privacy.open-app"), "", "", 0, true,
          [this, &inst, app, kind = cameraView ? PrivacyCaptureKind::Camera : PrivacyCaptureKind::Screen] {
            const bool camera = kind == PrivacyCaptureKind::Camera;
            const auto& sessions = camera ? m_cameraSessions : m_screenSessions;
            if (!sessions.sessions().contains(app) || !m_pipewire)
              return;
            std::vector<std::string> identities;
            for (const auto& capture : m_pipewire->privacyState().captures)
              if (capture.kind == kind && capture.appName == app && !capture.binary.empty())
                identities.push_back(capture.binary);
            identities.push_back(app);
            bool opened = false;
            for (const auto& identity : identities) {
              // Use the same keyboard-grab handoff as the transfer app controls.
              if (activateTransferSource(inst, {.desktopId = identity, .wmClass = identity})) {
                opened = true;
                break;
              }
            }
            (camera ? inst.cameraFeedback : inst.captureFeedback) = opened ? "" : app;
            refresh();
          },
          12
      );
      open->inputArea()->setTabFocusKey(std::string(cameraView ? "camera-open-" : "capture-open-") + app);
      pill(open);
      h += 66;
      if (captureFeedback == app) {
        label(i18n::tr("utilities.privacy.no-window"), 22, h - 4, w - 44, 11, muted);
        h += 24;
      }
    }
  }
  if (view == island::View::Microphone) {
    if (microphoneInputs.empty()) {
      label(i18n::tr("island.microphone.unavailable"), 22, h, w - 44, 12, muted, true);
      h += 32;
    }
    for (const auto& input : microphoneInputs) {
      const float top = h;
      label(audioDeviceLabel(input), 22, h + 5, w - 114, 13, foreground, false, 1, FontWeight::SemiBold, true);
      label(
          i18n::tr(input.muted ? "island.microphone.muted" : "island.microphone.live"), 22, h + 27, w - 114, 11,
          islandTint(input.muted ? kAppleRed : kAppleOrange, input.muted ? ColorRole::Error : ColorRole::Primary)
      );
      auto meter = std::make_unique<IslandMicrophoneMeter>(*m_pipewire, input);
      meter->setFill(islandTint(kAppleOrange, ColorRole::Primary));
      meter->setTrack(islandRole(ColorRole::OnSurface, 0.16F));
      meter->setRadius(2 * s);
      meter->setSize((w - 114) * s, 4 * s);
      meter->setPosition(22 * s, (h + 48) * s);
      inst.microphoneMeters.push_back(meter.get());
      canvas->addChild(std::move(meter));
      auto* muteButton = control(
          w - 66, h + 9, 40, 40, "", input.muted ? "microphone-off" : "microphone",
          i18n::tr(input.muted ? "island.microphone.unmute" : "island.microphone.mute"), 20, true,
          [this, id = input.id, name = input.name] {
            // Re-read the device at activation, including changes made outside the Island.
            const auto& sources = m_pipewire->state().sources;
            const auto source = std::ranges::find(sources, id, &AudioNode::id);
            if (source == sources.end() || source->name != name)
              return;
            m_osdQuietUntil = std::chrono::steady_clock::now() + 500ms;
            m_pipewire->setSourceMuted(id, !source->muted);
          }
      );
      muteButton->inputArea()->setTabFocusKey("microphone-mute-" + std::to_string(input.id));
      roundButton(muteButton, input.muted ? kAppleRed : kAppleOrange);
      h += 68;
      sectionCard(top, h);
    }
  }
  if (expandedView
      && cfg.hoverShowTimers
      && !timers.empty()
      && (showExtras || view == island::View::Timers)
      && (!inst.activities.switching || view == island::View::Timers)) {
    auto displayTimers = timers;
    // Compact priority can change on pause; keep hover buttons under the same pointer.
    std::ranges::sort(displayTimers, {}, &island::Countdown::plugin);
    for (const auto& timer : displayTimers) {
      if (!timer.active)
        continue;
      const float sectionTop = h;
      const Color tint = countdownTint(timer);
      if (gCupertino && !timer.event) {
        const float diameter = cfg.compactLayout ? 38.0F : 44.0F;
        auto ring = std::make_unique<DownloadRing>(
            diameter * s, 2.5F * s, timer.fraction(), islandTint(tint, ColorRole::Primary)
        );
        auto* ringPtr = ring.get();
        ring->setPosition(24 * s, (h + 13) * s);
        canvas->addChild(std::move(ring));
        glyph(
            timer.icon, 24 + (diameter - 22) / 2, h + 13 + (diameter - 22) / 2, 22, islandTint(tint, ColorRole::Primary)
        );
        const float timeX = 24 + diameter + 18;
        const float buttonSize = cfg.compactLayout ? 36.0F : 42.0F;
        const float controlsX = w - 24 - 2 * buttonSize - 10;
        const float timeWidth = std::max(1.0F, controlsX - timeX - 12);
        const float timeFont = cfg.compactLayout ? 28 : 34;
        const auto timeMetrics = renderer.measureText(
            countdownTime(timer), timeFont * s, FontWeight::SemiBold, 0, 1, TextAlign::Start,
            m_config->config().shell.fontFamily
        );
        const float fittedFont = timeFont * std::min(1.0F, timeWidth * s / std::max(1.0F, timeMetrics.width));
        auto* remaining = label(
            countdownTime(timer), timeX, h + 4, timeWidth, fittedFont, islandTint(tint, ColorRole::Primary), false, 1,
            FontWeight::SemiBold
        );
        inst.timerUi.push_back({timer.plugin, remaining, [ringPtr](float value) { ringPtr->setProgress(value); }});
        const auto detail = countdownTitle(timer)
            + (timer.running ? "" : " · " + i18n::tr(timer.finished ? "island.timer.finished" : "island.timer.paused"));
        label(detail, timeX, h + 46, std::max(1.0F, controlsX - timeX - 12), 13, muted);
        auto* toggle = control(
            controlsX, h + 16, buttonSize, buttonSize, "", timer.running ? "media-pause" : "media-play",
            i18n::tr(timer.running ? "island.timer.pause" : "island.timer.resume"), 18,
            !timer.finished && timer.remaining > 0, [this, timer] { timerCommand(timer, timer.toggleCommand()); }
        );
        toggle->inputArea()->setTabFocusKey(timer.plugin + "-toggle");
        roundButton(toggle);
        auto* cancel = control(
            controlsX + buttonSize + 10, h + 16, buttonSize, buttonSize, "", "x", i18n::tr("island.timer.cancel"), 18,
            true, [this, timer] { timerCommand(timer, timer.cancelCommand()); }
        );
        cancel->inputArea()->setTabFocusKey(timer.plugin + "-cancel");
        roundButton(cancel);
        h += 86;
        continue;
      }
      auto ring =
          std::make_unique<DownloadRing>(36 * s, 2.5F * s, timer.fraction(), islandTint(tint, ColorRole::Primary));
      auto* ringPtr = ring.get();
      ring->setPosition(22 * s, (h + 6) * s);
      canvas->addChild(std::move(ring));
      glyph(timer.icon, 31, h + 15, 18, islandTint(tint, ColorRole::Primary));
      // Cupertino keeps the controls on the timer's row as round buttons; the theme look
      // lists them as a row of text buttons below.
      const float controlsWidth = gCupertino ? 3 * 32 + 2 * 8 + 10 : 0;
      if (timer.event) {
        // The title takes the row; the countdown reads as its status line.
        label(countdownTitle(timer), 70, h + 3, w - 92 - controlsWidth, 13, foreground, false, 1, FontWeight::SemiBold);
        auto* status = label(eventStatus(timer), 70, h + 25, w - 92 - controlsWidth, 11, muted);
        inst.timerUi.push_back({timer.plugin, status, [ringPtr](float value) { ringPtr->setProgress(value); }, true});
      } else {
        label(countdownTitle(timer), 70, h + 3, w - 165 - controlsWidth, 13);
        auto* remaining = label(countdownTime(timer), w - 94 - controlsWidth, h + 3, 72, 16, foreground, true);
        inst.timerUi.push_back({timer.plugin, remaining, [ringPtr](float value) { ringPtr->setProgress(value); }});
        label(
            i18n::tr(
                timer.finished      ? "island.timer.finished"
                    : timer.running ? "island.timer.running"
                                    : "island.timer.paused"
            ),
            70, h + 25, w - 92, 11, muted
        );
      }
      // Events trade pause and cancel for joining the call and dismissing the countdown.
      const auto join = [url = timer.url] { (void)net::openInBrowser(url); };
      const auto dismiss = [this, key = timer.plugin] {
        m_dismissedEvents.insert(key);
        refresh();
      };
      if (gCupertino && timer.event) {
        const float x = w - 22 - controlsWidth + 10;
        auto* joinButton =
            control(x, h + 8, 32, 32, "", "video", i18n::tr("island.up-next.join"), 16, !timer.url.empty(), join);
        joinButton->inputArea()->setTabFocusKey(timer.plugin + "-join");
        roundButton(joinButton, kAppleGreen);
        auto* dismissButton =
            control(x + 40, h + 8, 32, 32, "", "x", i18n::tr("island.up-next.dismiss"), 16, true, dismiss);
        dismissButton->inputArea()->setTabFocusKey(timer.plugin + "-dismiss");
        roundButton(dismissButton);
        auto* open = control(
            x + 80, h + 8, 32, 32, "", "chevron-right", i18n::tr("island.up-next.open"), 16, true,
            [panel, timer] { panel(timer.panel); }
        );
        open->inputArea()->setTabFocusKey(timer.plugin + "-open");
        roundButton(open);
        h += 56;
        sectionCard(sectionTop, h);
        continue;
      }
      if (timer.event) {
        h += 48;
        const float buttonWidth = (w - 60) / 3;
        auto* joinButton =
            control(22, h, buttonWidth, 30, i18n::tr("island.up-next.join"), "", "", 0, !timer.url.empty(), join);
        joinButton->inputArea()->setTabFocusKey(timer.plugin + "-join");
        auto* dismissButton =
            control(30 + buttonWidth, h, buttonWidth, 30, i18n::tr("island.up-next.dismiss"), "", "", 0, true, dismiss);
        dismissButton->inputArea()->setTabFocusKey(timer.plugin + "-dismiss");
        auto* open = control(
            38 + 2 * buttonWidth, h, buttonWidth, 30, i18n::tr("island.up-next.open"), "", "", 0, true,
            [panel, timer] { panel(timer.panel); }
        );
        open->inputArea()->setTabFocusKey(timer.plugin + "-open");
        h += 38;
        sectionCard(sectionTop, h);
        continue;
      }
      h += 48;
      const float buttonWidth = (w - 60) / 3;
      auto* toggle = control(
          22, h, buttonWidth, 30, i18n::tr(timer.running ? "island.timer.pause" : "island.timer.resume"), "", "", 0,
          !timer.finished && timer.remaining > 0, [this, timer] { timerCommand(timer, timer.toggleCommand()); }
      );
      toggle->inputArea()->setTabFocusKey(timer.plugin + "-toggle");
      auto* cancel = control(
          30 + buttonWidth, h, buttonWidth, 30, i18n::tr("island.timer.cancel"), "", "", 0, true,
          [this, timer] { timerCommand(timer, timer.cancelCommand()); }
      );
      cancel->inputArea()->setTabFocusKey(timer.plugin + "-cancel");
      auto* open = control(
          38 + 2 * buttonWidth, h, buttonWidth, 30, i18n::tr("island.timer.open"), "", "", 0, true,
          [panel, timer] { panel(timer.panel); }
      );
      open->inputArea()->setTabFocusKey(timer.plugin + "-open");
      h += 38;
      sectionCard(sectionTop, h);
    }
  }
  // The expanded Island's icon row ends with the unread-notifications bell where the unread
  // section itself is not shown (the Cupertino Island keeps it to the calendar view), unless
  // the hover view's unread section is turned off.
  const bool rowBell = expandedView && showUnread && cfg.hoverShowUnread && !showExtras;
  const bool mediaTray = contextualHeader || (view == island::View::Media && !showExtras);
  const bool mediaIndicator = contextualHeader && playing;
  const bool recordingIndicator = contextualHeader && recording;
  const int leadingIndicators = int(mediaIndicator) + int(recordingIndicator);
  const float statusWidth = static_cast<float>(privacyList.size() + (rowBell ? 1 : 0) + leadingIndicators) * 24;
  const float trayGap = statusWidth > 0 ? 8.0F : 0.0F;
  const bool showHoverWidgets = expandedView
      && (showExtras || view == island::View::Media || contextualHeader)
      && m_widgetFactory
      && (cfg.hoverShowTray
          || !cfg.hoverWidgets.empty()
          || !cfg.hoverWidgetsCenter.empty()
          || !cfg.hoverWidgetsRight.empty());
  if (showHoverWidgets) {
    if (!retainedWidgets) {
      retainedWidgets = std::make_unique<IslandWidgetHost>(
          *m_widgetFactory, m_config->config(), inst.output, s, &inst.animations, &m_widgetActions,
          [&inst] {
            if (!inst.panelHosted)
              inst.surface->requestUpdate();
          },
          [&inst] {
            if (!inst.panelHosted)
              inst.surface->requestRedraw();
          },
          [&inst] {
            if (!inst.panelHosted)
              inst.surface->requestFrameTick();
          },
          cfg, inst.barConfig.name, !showExtras
      );
    }
    inst.hoverWidgets = static_cast<IslandWidgetHost*>(retainedWidgets.get());
    const float availableWidth = contextualHeader ? std::max(24.0F, std::min(120.0F, w - 200 - statusWidth))
                                                  : w - 44 - (mediaTray ? statusWidth + trayGap : 0);
    inst.hoverWidgets->updateWidgets(renderer, std::max(1.0F, availableWidth * s));
  }
  const bool inlineTray = mediaTray && inst.hoverWidgets && inst.hoverWidgets->height() > 0;
  const float trayWidth = inlineTray ? inst.hoverWidgets->contentWidth() / s : 0;
  const float statusHeight = inlineTray ? std::max(24.0F, inst.hoverWidgets->height() / s) : 24.0F;
  if (contextualHeader || !privacyList.empty() || rowBell || inlineTray) {
    Node* statusCanvas = canvas;
    if (contextualHeader)
      canvas = inst.content;
    // Capture indicators are clickable icons in compact views and beside notifications,
    // and as a centred row in the expanded Island. Hovering names the capturing app.
    const float rowWidth = statusWidth + (inlineTray ? trayGap + trayWidth : 0);
    const float x = compactView
        ? w - (showUnread ? (view == island::View::Activity ? 48 : 38) : 14) - (showBattery ? 42 : 0) - privacyWidth
        : contextualHeader ? w - 22 - rowWidth
                           : (w - rowWidth) / 2;
    const float y = compactView ? (cfg.height - 24) / 2
        : contextualHeader      ? 16
                                : h + (inlineTray ? 8 + (statusHeight - 24) / 2 : 0);
    const auto dot = [&](float left, ColorSpec color, const std::string& tooltip, bool fromArtwork = false) {
      constexpr float diameter = 7;
      const float dotX = left + (24 - diameter) / 2, dotY = y + (24 - diameter) / 2;
      auto circle = std::make_unique<Box>();
      if (fromArtwork && m_flow.hasArtwork()) {
        const auto accent = m_flow.accent();
        RoundedRectStyle gradient;
        gradient.fillMode = FillMode::LinearGradient;
        gradient.gradientDirection = GradientDirection::Vertical;
        const Color top = rgba(accent.r * .65F + .35F, accent.g * .65F + .35F, accent.b * .65F + .35F);
        const Color bottom = rgba(accent.r, accent.g, accent.b);
        gradient.gradientStops = {{{0, top}, {1, bottom}, {1, bottom}, {1, bottom}}};
        circle->setStyle(gradient);
      } else {
        circle->setFill(color);
      }
      circle->setRadius(diameter * s / 2);
      circle->setSize(diameter * s, diameter * s);
      circle->setPosition(dotX * s, dotY * s);
      circle->setHitTestVisible(false);
      canvas->addChild(std::move(circle));
      auto hint = std::make_unique<InputArea>();
      hint->setAcceptedButtons(0);
      hint->setSize(24 * s, 24 * s);
      hint->setPosition(left * s, y * s);
      hint->setTooltip(tooltip);
      canvas->addChild(std::move(hint));
    };
    if (mediaIndicator)
      dot(x, muted, i18n::tr("island.activities.now-playing"), true);
    if (recordingIndicator)
      dot(x + (mediaIndicator ? 24 : 0), islandFixed(kAppleRed, 1), i18n::tr("island.capture.recording"));
    if (contextualHeader) {
      const auto activity = view == island::View::Media ? island::Activity::Media
          : view == island::View::Downloads             ? island::Activity::Downloads
          : view == island::View::Timers                ? island::Activity::Timers
          : view == island::View::Microphone            ? island::Activity::Microphone
          : view == island::View::Awake                 ? island::Activity::Awake
          : view == island::View::Camera                ? island::Activity::Camera
                                                        : island::Activity::Capture;
      const auto titleFor = [&](island::Activity item) {
        switch (item) {
        case island::Activity::Media:
          return i18n::tr("island.activities.now-playing");
        case island::Activity::Downloads:
          return i18n::tr("island.activities.downloads");
        case island::Activity::Timers:
          return i18n::tr("island.activities.timers");
        case island::Activity::Microphone:
          return i18n::tr("island.microphone.title");
        case island::Activity::Awake:
          return i18n::tr("utilities.keep-awake.title");
        case island::Activity::Camera:
          return i18n::tr("bar.widgets.privacy.camera");
        case island::Activity::Capture:
          return i18n::tr(recording ? "island.capture.recording" : "bar.widgets.privacy.screen-sharing");
        default:
          return std::string{};
        }
      };
      const auto next = island::nextActivity(cyclingActivities, activity);
      const auto title = titleFor(activity);
      const float font = cfg.compactLayout ? 14 : 16;
      const float titleWidth = std::max(
          1.0F,
          std::min(
              x - 34,
              renderer.measureText(
                          title, font * s, FontWeight::Normal, 0, 1, TextAlign::Start,
                          m_config->config().shell.fontFamily
              )
                          .width
                      / s
                  + 4
          )
      );
      if (next != island::Activity::None && next != activity) {
        const auto switchActivity = [this, &inst, next] {
          TooltipManager::instance().forceDestroy();
          inst.activities.selected = next;
          inst.recentTransfersExpanded = false;
          if (inst.config.splitActivities
              && (next == island::Activity::Media
                  || next == island::Activity::Downloads
                  || next == island::Activity::Timers
                  || next == island::Activity::Awake))
            inst.compactActivity.promote(next);
          refresh();
        };
        auto* heading = control(
            22, 12, titleWidth, 32, title, "", i18n::tr("island.activities.switch-to", "activity", titleFor(next)), 0,
            true, switchActivity, font, 0
        );
        // Use the Island's stable action target across card rebuilds; keyboard activation
        // continues through the button's focus key.
        heading->inputArea()->setAcceptedButtons(0);
        action(22, 12, titleWidth, 32, "activity-title", switchActivity);
        heading->inputArea()->setTabFocusKey("activity-title");
        setIslandStatusStyle(heading, muted);
      } else {
        auto* heading = label(title, 22, 12, titleWidth, font, muted);
        heading->setPosition(22 * s, (28 * s - heading->height() / 2));
      }
    }
    for (std::size_t i = 0; i < privacyList.size(); ++i) {
      const auto& activity = privacyList[i];
      if (contextualHeader) {
        const float left = x + static_cast<float>(i + leadingIndicators) * 24;
        const auto tooltip = i18n::tr(activity.labelKey()) + ": " + activity.appNames();
        if (activity.kind == PrivacyCaptureKind::Screen) {
          glyph(activity.icon(), left + 4, y + 4, 16, islandTint(kApplePurple, ColorRole::Primary));
          auto hint = std::make_unique<InputArea>();
          hint->setAcceptedButtons(0);
          hint->setSize(24 * s, 24 * s);
          hint->setPosition(left * s, y * s);
          hint->setTooltip(tooltip);
          canvas->addChild(std::move(hint));
        } else {
          dot(left, islandFixed(activity.kind == PrivacyCaptureKind::Camera ? kAppleGreen : kAppleOrange, 1), tooltip);
        }
        continue;
      }
      const bool privacyDot = gCupertino && !slotBell && activity.kind != PrivacyCaptureKind::Screen;
      auto* icon = control(
          x + static_cast<float>(i) * 24, y, 24, 24, "",
          slotBell         ? "notification-unread"
              : privacyDot ? "circle-filled"
                           : activity.icon(),
          slotBell ? i18n::tr("notifications.unread-history")
                   : i18n::tr(activity.labelKey()) + ": " + activity.appNames(),
          privacyDot ? 7 : 16, true,
          [panel, openMicrophone, openCapture, openCamera, slotBell, kind = activity.kind] {
            if (!slotBell && kind == PrivacyCaptureKind::Microphone)
              openMicrophone();
            else if (!slotBell && kind == PrivacyCaptureKind::Screen)
              openCapture();
            else if (!slotBell && kind == PrivacyCaptureKind::Camera)
              openCamera();
            else
              panel(slotBell ? "notifications" : "privacy");
          },
          10, 0
      );
      if (expandedView) {
        // In the expanded Island a button click that lends the surface to a panel loses the
        // panel's focus grab, so the icon keeps its hover and tooltip while the click falls
        // through to the Island's own action handling, as the media card's panel link does.
        icon->inputArea()->setAcceptedButtons(0);
        if (activity.kind == PrivacyCaptureKind::Microphone)
          action(x + static_cast<float>(i) * 24, y, 24, 24, "privacy-microphone", openMicrophone);
        else if (activity.kind == PrivacyCaptureKind::Screen)
          action(x + static_cast<float>(i) * 24, y, 24, 24, "privacy-screen", openCapture);
        else
          action(x + static_cast<float>(i) * 24, y, 24, 24, "privacy-camera", openCamera);
      }
      setIslandStatusStyle(
          icon,
          !slotBell && activity.kind == PrivacyCaptureKind::Microphone   ? islandTint(kAppleOrange, ColorRole::Primary)
              : !slotBell && activity.kind == PrivacyCaptureKind::Screen ? islandTint(kApplePurple, ColorRole::Primary)
              : !slotBell && activity.kind == PrivacyCaptureKind::Camera ? islandTint(kAppleGreen, ColorRole::Primary)
                                                                         : islandRole(ColorRole::Primary)
      );
      if (compactView) {
        // When the slot moves on, the outgoing icon rises and fades as the next rises into place.
        const std::string slotIcon = slotBell ? "notification-unread" : activity.icon();
        if (!inst.slotIcon.empty() && inst.slotIcon != slotIcon) {
          auto ghost = std::make_unique<Glyph>();
          // The outgoing visual must not cover the incoming icon's pointer target.
          ghost->setHitTestVisible(false);
          const bool outgoingDot =
              gCupertino && (inst.slotIcon == "privacy-camera" || inst.slotIcon == "privacy-microphone");
          ghost->setGlyph(outgoingDot ? "circle-filled" : inst.slotIcon);
          ghost->setGlyphSize((outgoingDot ? 7 : 16) * s);
          ghost->setColor(
              inst.slotIcon == "privacy-camera"           ? islandTint(kAppleGreen, ColorRole::Primary)
                  : inst.slotIcon == "privacy-microphone" ? islandTint(kAppleOrange, ColorRole::Primary)
                  : inst.slotIcon == "privacy-screen"     ? islandTint(kApplePurple, ColorRole::Primary)
                                                          : islandRole(ColorRole::Primary)
          );
          ghost->measure(renderer);
          ghost->setPosition(
              icon->x() + (icon->width() - ghost->width()) / 2, icon->y() + (icon->height() - ghost->height()) / 2
          );
          auto* outgoing = static_cast<Glyph*>(canvas->addChild(std::move(ghost)));
          const float iconY = icon->y(), outgoingY = outgoing->y(), travel = 8 * s;
          icon->setOpacity(0);
          icon->setPosition(icon->x(), iconY + travel);
          inst.animations.animate(
              0, 1, Motion::resizeMs, Motion::reveal,
              [icon, outgoing, iconY, outgoingY, travel](float t) {
                // The incoming icon waits for the outgoing one to clear before it shows.
                icon->setOpacity(std::clamp((t - 0.25F) / 0.75F, 0.0F, 1.0F));
                icon->setPosition(icon->x(), iconY + (1 - t) * travel);
                outgoing->setOpacity(std::max(0.0F, 1 - 2 * t));
                outgoing->setPosition(outgoing->x(), outgoingY - t * travel);
              },
              {}, icon
          );
        }
        inst.slotIcon = slotIcon;
        icon->setOnEnter([&inst] {
          inst.badgeHovered = true;
          inst.enter.stop();
        });
        icon->setOnLeave([this, &inst] {
          inst.badgeHovered = false;
          if (inst.inside && !inst.hovered && !inst.suppressHover)
            inst.enter.start(std::chrono::milliseconds(inst.config.hoverOpenDelayMs), [this, &inst] {
              if (inst.inside && !inst.badgeHovered && !inst.splitHovered() && !cardActionKey(inst).has_value()) {
                inst.hovered = true;
                refresh();
              }
            });
        });
      }
    }
    if (rowBell) {
      const float bellX = x + static_cast<float>(privacyList.size() + leadingIndicators) * 24;
      auto* bell = control(
          bellX, y, 24, 24, "", "notification-unread", i18n::trp("notifications.unread-count", unreadCount), 16, true,
          [panel] { panel("notifications"); }, 10, 0
      );
      // As with the capture icons, the click goes through the Island's own action handling.
      bell->inputArea()->setAcceptedButtons(0);
      action(bellX, y, 24, 24, "unread-bell", [panel] { panel("notifications"); });
      setIslandStatusStyle(bell, islandRole(ColorRole::Primary));
    }
    if (inlineTray) {
      // The tray centers its contents in the remaining width; center the status icons
      // beside those contents so the complete row stays centered as tray items change.
      if (contextualHeader) {
        // The tray host centres its contents inside its allotted width.
        const float offset = (inst.hoverWidgets->width() / s - trayWidth) / 2;
        inst.hoverWidgets->setPosition(
            (x + statusWidth + trayGap - offset) * s, (y + 12) * s - inst.hoverWidgets->height() / 2
        );
        inst.content->addChild(std::move(retainedWidgets));
      } else {
        inst.hoverWidgets->setPosition((22 + statusWidth + trayGap) * s, (h + 8) * s);
        footer->addChild(std::move(retainedWidgets));
        h += statusHeight + 16;
      }
    } else if (!compactView && !contextualHeader)
      h += 32;
    canvas = statusCanvas;
  }
  const auto batteryRing = [&](const island::Battery& battery, float x, float y, float diameter, bool drawRing = true) {
    const auto role = battery.low ? ColorRole::Error : ColorRole::Primary;
    const ColorSpec ringFill = battery.low ? islandRole(ColorRole::Error) : islandTint(kAppleGreen, ColorRole::Primary);
    if (drawRing) {
      auto ring = std::make_unique<DownloadRing>(
          diameter * s, 2.5F * s, static_cast<float>(battery.percentage / 100.0), ringFill, battery.charging()
      );
      ring->setPosition(x * s, y * s);
      canvas->addChild(std::move(ring));
    }
    glyph(
        battery.charging() ? "battery-charging" : battery.icon, x + (diameter - 18) / 2, y + (diameter - 18) / 2, 18,
        islandRole(role)
    );
  };
  if (showBattery)
    batteryRing(batteryList.front(), w - 50 - (showUnread ? 36 : 0), (cfg.height - 36) / 2, 36);
  if (expandedView && showExtras && cfg.hoverShowBatteries && !batteryList.empty()) {
    const float sectionTop = h;
    const auto rows = std::min(batteryList.size(), std::size_t{4});
    for (std::size_t i = 0; i < rows; ++i) {
      const auto& battery = batteryList[i];
      batteryRing(battery, 22, h + 7, 36);
      label(battery.name, 70, h + 4, w - 150, 13, foreground, false, 1, FontWeight::Normal, true);
      label(
          std::format("{}%", std::lround(battery.percentage)), w - 76, h + 4, 54, 13,
          islandRole(battery.low ? ColorRole::Error : ColorRole::OnSurface), true
      );
      auto detail = batteryStateLabel(battery.state);
      if (battery.seconds > 0)
        detail += " · "
            + formatDuration(std::chrono::seconds(battery.seconds))
            + " "
            + i18n::tr(battery.charging() ? "island.battery.until-full" : "island.battery.remaining");
      label(detail, 70, h + 26, w - 92, 11, muted);
      h += 54;
    }
    if (batteryList.size() > rows) {
      label(i18n::trp("island.battery.more", batteryList.size() - rows), 22, h, w - 44, 12, muted);
      h += 26;
    }
    h += 6;
    sectionCard(sectionTop, h);
  }
  if (showUnread && expandedView && showExtras && cfg.hoverShowUnread && gCupertino) {
    // macOS Notification Centre: the newest unread notification as its own card (app icon, app
    // name and time, title, two lines of body) with the older ones stacked behind its lower edge.
    // Clicking the stack opens the history.
    std::vector<const NotificationHistoryEntry*> unread;
    for (const auto& entry : m_notifications->history() | std::views::reverse)
      if (!entry.seen)
        unread.push_back(&entry);
    const auto& note = unread.front()->notification;
    const auto flatten = [](std::string text) {
      std::replace(text.begin(), text.end(), '\n', ' ');
      std::replace(text.begin(), text.end(), '\r', ' ');
      return text;
    };
    constexpr float cardX = 12.0F;
    constexpr float iconSize = 32.0F;
    constexpr float timeWidth = 56.0F;
    const float cardTop = h + 2;
    const float cardWidth = w - 2 * cardX;
    const bool hasIcon = notificationIcon(note, cardX + 12, cardTop + 12, iconSize);
    const float textX = hasIcon ? cardX + 12 + iconSize + 10 : cardX + 14;
    const float textWidth = cardX + cardWidth - 14 - textX;
    label(note.appName, textX, cardTop + 10, textWidth - timeWidth - 4, Style::fontSizeCaption, muted);
    auto* time = label(
        formatNotificationTime(note.receivedWallClock.value_or(WallClock::now())), textX + textWidth - timeWidth,
        cardTop + 10, timeWidth, Style::fontSizeCaption, muted
    );
    time->setTextAlign(TextAlign::End);
    time->measure(renderer);
    auto* title = label(
        flatten(note.summary), textX, cardTop + 27, textWidth, Style::fontSizeBody, foreground, false, 1,
        FontWeight::SemiBold
    );
    float textBottom = cardTop + 27 + title->height() / s;
    if (note.body.find_first_not_of(" \t\r\n") != std::string::npos) {
      auto* body = label(flatten(note.body), textX, textBottom + 1, textWidth, Style::fontSizeBody, muted, false, 2);
      textBottom += 1 + body->height() / s;
    }
    const float cardBottom = std::max(cardTop + 12 + iconSize, textBottom) + 12;
    const float cardHeight = cardBottom - cardTop;
    // Up to two cards peek out beneath, each a little narrower and fainter, as a collapsed stack.
    const std::size_t behind = std::min<std::size_t>(unread.size() - 1, 2);
    constexpr float peek = 7.0F;
    const auto card = [&](Node& parent, float x, float y, float width, ColorSpec fill, float alpha, int z) {
      auto box = std::make_unique<Box>();
      box->setFill(fill);
      box->setBorder(islandRole(ColorRole::Outline, 0.5F * alpha), Style::borderWidth);
      box->setRadius(Style::scaledRadiusLg(s) * 1.25F);
      box->setPosition(x * s, y * s);
      box->setSize(width * s, cardHeight * s);
      box->setHitTestVisible(false);
      box->setZIndex(z);
      parent.addChild(std::move(box));
    };
    // Every card is translucent (glass), so each back card is clipped to its own band: the strip
    // between the bottom edge of the card in front of it and its own bottom edge. Only the part
    // that peeks out is drawn, never anything under another card.
    for (std::size_t i = 1; i <= behind; ++i) {
      const float bandTop = cardBottom + peek * static_cast<float>(i - 1);
      auto band = std::make_unique<Box>();
      band->setFill(clearColorSpec());
      band->clearBorder();
      band->setClipChildren(true);
      band->setHitTestVisible(false);
      band->setPosition(0, bandTop * s);
      band->setSize(w * s, (peek + 1) * s);
      band->setZIndex(-2);
      auto* bandNode = canvas->addChild(std::move(band));
      const float inset = 10.0F * static_cast<float>(i);
      const float alpha = i == 1 ? 0.7F : 0.45F;
      card(
          *bandNode, cardX + inset, cardTop + peek * static_cast<float>(i) - bandTop, cardWidth - 2 * inset,
          islandRole(ColorRole::SurfaceVariant, alpha), alpha, 0
      );
    }
    card(*canvas, cardX, cardTop, cardWidth, islandRole(ColorRole::SurfaceVariant), 1.0F, -1);
    h = cardBottom + peek * static_cast<float>(behind) + 4;
    if (unread.size() > 1) {
      label(
          i18n::trp("notifications.stack-more", unread.size() - 1), cardX, h, cardWidth, Style::fontSizeCaption, muted,
          true
      );
      h += 20;
    }
    action(cardX, cardTop, cardWidth, h - cardTop, "unread-stack", [panel] { panel("notifications"); });
  } else if (showUnread && expandedView && showExtras && cfg.hoverShowUnread) {
    const float sectionTop = h;
    // Cupertino: a leading bell badge and left-aligned rows, matching the battery and timer rows.
    const float rowX = gCupertino ? 58 : 22;
    if (gCupertino)
      leadingBadge("notification-unread", 22, h + 2, 28, kAppleRed, ColorRole::Error);
    auto* header = control(
        rowX, h + 4, w - rowX - 22, 24, i18n::trp("notifications.unread-count", unreadCount), "",
        i18n::tr("notifications.unread-history"), 0, true, [panel] { panel("notifications"); }
    );
    if (gCupertino)
      header->setContentAlign(ButtonContentAlign::Start);
    h += 36;
    std::size_t shown = 0;
    for (const auto& entry : m_notifications->history() | std::views::reverse) {
      if (entry.seen)
        continue;
      if (shown++ == 3)
        break;
      const auto& notification = entry.notification;
      auto title = notification.appName + " · " + notification.summary;
      std::replace(title.begin(), title.end(), '\n', ' ');
      std::replace(title.begin(), title.end(), '\r', ' ');
      auto* row = control(rowX, h, w - rowX - 22, 30, title, "", notification.body, 0, true, [panel] {
        panel("notifications");
      });
      if (gCupertino)
        row->setContentAlign(ButtonContentAlign::Start);
      h += 34;
    }
    sectionCard(sectionTop, h);
  }
  if (showUnread
      && (view == island::View::Rest
          || view == island::View::Activity
          || view == island::View::DownloadActivity
          || view == island::View::AwakeActivity
          || view == island::View::TimerActivity)) {
    const float badgeX = view == island::View::Activity ? w - 44 : w - badgeWidth - 10;
    const float badgeY = (cfg.height - 24) / 2;
    auto* badge = control(
        badgeX, badgeY, badgeWidth, 24, "", "notification-unread", i18n::tr("notifications.unread-history"), 16, true,
        [panel] { panel("notifications"); }, 10, 0
    );
    badge->setRadius(Style::scaledRadius(12, s));
    setIslandStatusStyle(badge, islandRole(ColorRole::Primary));
    badge->setOnEnter([&inst] {
      inst.badgeHovered = true;
      inst.enter.stop();
    });
    badge->setOnLeave([this, &inst] {
      inst.badgeHovered = false;
      if (inst.inside && !inst.hovered && !inst.suppressHover)
        inst.enter.start(std::chrono::milliseconds(inst.config.hoverOpenDelayMs), [this, &inst] {
          if (inst.inside && !inst.badgeHovered && !inst.splitHovered() && !cardActionKey(inst).has_value()) {
            inst.hovered = true;
            refresh();
          }
        });
    });
  }
  if (view == island::View::Calendar && cfg.hoverShowDownloads && !recentTransfers.empty()) {
    auto* recent =
        control(22, h + 8, w - 44, 32, i18n::tr("island.downloads.recent"), "download", "", 16, true, [this, &inst] {
          inst.activities.selected = island::Activity::Downloads;
          inst.recentTransfersExpanded = true;
          refresh();
        });
    recent->inputArea()->setTabFocusKey("recent-transfers");
    pill(recent);
    h += 48;
  }
  if (view == island::View::Downloads && gCupertino && !downloads.empty() && !recentTransfers.empty()) {
    auto* recent = control(
        22, h + 6, w - 44, 32,
        i18n::tr(inst.recentTransfersExpanded ? "island.downloads.hide-recent" : "island.downloads.recent"), "", "", 0,
        true, [this, &inst] {
          inst.recentTransfersExpanded = !inst.recentTransfersExpanded;
          refresh();
        }
    );
    recent->inputArea()->setTabFocusKey("recent-transfers");
    h += 44;
  }
  if (view == island::View::Downloads && !gCupertino) {
    auto* close = control(22, h, w - 44, 32, i18n::tr("island.downloads.close"), "", "", 0, true, [this, &inst] {
      if (inst.keyboardMode)
        releaseKeyboard(inst);
      else {
        inst.hovered = false;
        inst.suppressHover = inst.inside;
        inst.enter.stop();
        refresh();
      }
    });
    pill(close);
    h += 42;
  }
  if (showHoverWidgets && retainedWidgets) {
    inst.hoverWidgets->setPosition(22 * s, (h + 8) * s);
    const float widgetHeight = inst.hoverWidgets->height() / s;
    footer->addChild(std::move(retainedWidgets));
    if (widgetHeight > 0) {
      // A card groups a row of widgets; the tray alone (often one or two icons) sits on the Island
      // like the calendar above it, rather than on a mostly empty bar.
      if (!inst.hoverWidgets->trayOnly())
        sectionCard(h, h + widgetHeight + 16);
      h += widgetHeight + 16;
    }
  }
  if (expandedView) {
    const float footerHeight = h - footerTop;
    for (const auto& child : footer->children())
      child->setPosition(child->x(), child->y() - footerTop * s);
    footer->setSize(w * s, footerHeight * s);
    const float available =
        std::max(1.0F, inst.outputHeight / s - footerTop - 24 - kExpandedBottomInset - 2 * cardGutter);
    if (footerHeight > available) {
      auto scroll = std::make_unique<ScrollView>();
      scroll->setContentScale(s);
      scroll->setViewportPaddingH(0);
      scroll->setViewportPaddingV(0);
      scroll->setScrollbarInsetV(12 * s);
      scroll->content()->addChild(std::move(footer));
      scroll->setSize(w * s, available * s);
      scroll->setPosition(0, footerTop * s);
      inst.activityScroll = scroll.get();
      inst.content->addChild(std::move(scroll));
      h = footerTop + available;
    } else {
      footer->setPosition(0, footerTop * s);
      inst.content->addChild(std::move(footer));
    }
  }
  // Cards keep their 4 px trailing gap; the inset brings the bottom margin up to the 12 px side
  // margin so a card ending the content stays inside the Island's large lower corners.
  if (expandedView)
    h = std::max(h + kExpandedBottomInset, cfg.height);
  if (cardGutter > 0) {
    for (const auto& child : inst.content->children())
      child->setPosition(child->x() + cardGutter * s, child->y() + cardGutter * s);
    for (auto& hit : inst.actions) {
      hit.x += cardGutter;
      hit.y += cardGutter;
    }
    if (inst.seek) {
      inst.seekX += cardGutter;
      inst.seekY += cardGutter;
    }
    w += 2 * cardGutter;
    h += 2 * cardGutter;
  }
  inst.content->setSize(w * s, h * s);
  inst.content->layout(renderer);
  if (inst.activityScroll)
    inst.activityScroll->setScrollOffset(activityOffset);
  if (inst.keyboardMode) {
    inst.input.restoreTabFocus(keyboardFocus);
    if (!inst.input.focusedArea()) {
      // Removed rows and closed source windows release the grab rather than
      // moving Enter/Space onto the next app or a media control.
      if (keyboardFocus.key
          && (keyboardFocus.key->starts_with("download:")
              || keyboardFocus.key->starts_with("recent-transfer:")
              || keyboardFocus.key->starts_with("capture-open-")
              || keyboardFocus.key->starts_with("camera-open-")
              || *keyboardFocus.key == "capture-stop")) {
        releaseKeyboard(inst);
        return;
      }
      (void)inst.input.cycleTabFocus(false);
    }
  }
  if (w != inst.targetWidth || h != inst.targetHeight) {
    inst.animations.cancel(inst.morph);
    const float oldW = inst.width, oldH = inst.height;
    inst.targetWidth = w;
    inst.targetHeight = h;
    const bool growing = w > oldW || h > oldH;
    const Motion::Spring spring = growing ? Motion::islandExpand : Motion::islandCollapse;
    const float durationMs = Motion::settleMs(spring);
    // Each axis springs over its own travel, starting at the speed the last morph left it with.
    const auto travelVelocity = [](float velocity, float travel) {
      return std::abs(travel) > 0.5F ? velocity / travel : 0.0F;
    };
    const float widthStart = travelVelocity(inst.widthVelocity, w - oldW);
    const float heightStart = travelVelocity(inst.heightVelocity, h - oldH);
    inst.morph = inst.animations.animate(
        0, 1, durationMs, Easing::Linear,
        [this, &inst, spring, durationMs, widthStart, heightStart, oldW, oldH, w, h](float value) {
          const auto across = Motion::spring(spring, value * durationMs, widthStart);
          const auto down = Motion::spring(spring, value * durationMs, heightStart);
          // Land exactly on the target; the spring's remaining motion is under 0.1% by then.
          const bool settled = value >= 1.0F;
          inst.width = settled ? w : oldW + (w - oldW) * across.position;
          inst.height = settled ? h : oldH + (h - oldH) * down.position;
          inst.widthVelocity = settled ? 0 : (w - oldW) * across.velocity;
          inst.heightVelocity = settled ? 0 : (h - oldH) * down.velocity;
          geometry(inst);
        }
    );
  }
  geometry(inst);
}

void Island::fitSurface(Instance& inst) {
  if (inst.panelHosted || inst.outputHeight <= 0)
    return;
  // Grow as soon as the capsule heads somewhere taller; shrink only once it has settled, so a
  // collapse never runs into the surface's edge.
  const auto max = static_cast<std::uint32_t>(inst.outputHeight);
  const auto want = std::min(max, surfaceHeightFor(std::max(inst.height, inst.targetHeight), inst.scale));
  const bool settled = inst.width == inst.targetWidth && inst.height == inst.targetHeight;
  if (want > inst.surfaceHeight || (settled && want < inst.surfaceHeight)) {
    inst.surfaceHeight = want;
    inst.surface->requestSize(inst.surfaceWidth, want);
  }
}

void Island::clearCrossfade(Instance& inst) {
  inst.animations.cancel(inst.contentFadeAnimation);
  inst.contentFadeAnimation = 0;
  inst.crossfadeSerial = ++m_crossfadeSerial;
  if (inst.outgoing) {
    (void)inst.background->removeChild(inst.outgoing);
    inst.outgoing = nullptr;
  }
  inst.outgoingFade = 0;
  inst.contentFade = 1;
}

void Island::crossfadeOut(Instance& inst, std::unique_ptr<Node> previous, bool cardChanged) {
  // While the outgoing layer is still dominant, replace only the incoming one.
  // Bursts converge on the latest content without restarting the fade or queuing cards.
  if ((inst.outgoing && inst.outgoing->opacity() >= previous->opacity()) || previous->opacity() <= 0.01F)
    return;
  clearCrossfade(inst);
  const auto serial = inst.crossfadeSerial;
  previous->setHitTestVisible(false);
  previous->setExcludeSubtreeFromTabOrder(true);
  // Behind the incoming content, which is added after it.
  inst.outgoing = inst.background->addChild(std::move(previous));
  Node* outgoing = inst.outgoing;
  inst.outgoingWidth = inst.targetWidth; // not yet the new view's
  const float startOpacity = outgoing->opacity();
  inst.outgoingFade = startOpacity > 0.01F ? 1.0F : 0.0F;
  inst.animations.animate(
      0, 1, cardChanged ? Motion::feedbackMs : kViewFadeOutMs, Easing::EaseOutCubic,
      [this, &inst, outgoing, startOpacity](float t) {
        outgoing->setOpacity(startOpacity * (1 - t));
        if (inst.outgoing == outgoing) {
          inst.outgoingFade = startOpacity > 0.01F ? 1 - t : 0.0F;
          geometry(inst);
        }
      },
      [this, &inst, outgoing, serial] {
        if (inst.outgoing != outgoing)
          return;
        inst.outgoingFade = 0.0F;
        geometry(inst);
        // Detach outside the animation tick. A replacement may reuse the old node's address.
        DeferredCall::callLater([this, instance = &inst, outgoing, serial, alive = std::weak_ptr<void>(m_lifetime)] {
          if (alive.expired())
            return;
          for (auto& ptr : m_instances) {
            if (ptr.get() != instance || ptr->crossfadeSerial != serial || ptr->outgoing != outgoing)
              continue;
            ptr->outgoing = nullptr;
            (void)ptr->background->removeChild(outgoing);
          }
        });
      },
      outgoing
  );
  inst.contentFade = 0;
  inst.contentFadeAnimation = inst.animations.animate(
      0, 1, cardChanged ? Motion::contentMs : kViewFadeInMs, Easing::EaseOutCubic,
      [this, &inst](float t) {
        inst.contentFade = t;
        geometry(inst);
      },
      [&inst] { inst.contentFadeAnimation = 0; }
  );
}

void Island::updateFlow(Instance& inst, const std::string& artwork) {
  if (inst.flowImage == nullptr || inst.flowArt == artwork)
    return;
  inst.flowArt = artwork;
  if (!artwork.empty() && artwork != m_flowArt) {
    m_flowArt = artwork;
    auto art = loadImageFile(artwork, 32, true);
    if (!art || !m_flow.setArtwork(art->rgba, art->width, art->height))
      m_flow.clear();
  }
  inst.flowShown = !artwork.empty() && m_flow.hasArtwork();
  if (!inst.flowShown) {
    inst.flowImage->setVisible(false);
    syncFlowTimer();
    return;
  }
  m_renderContext->makeCurrent(inst.surface->renderTarget());
  m_flow.render(
      visuals::ArtworkFlow::frozen()
          ? 0.0F
          : std::chrono::duration<float>(std::chrono::steady_clock::now() - m_flowStart).count(),
      m_flowFrame
  );
  auto& textures = inst.surface->renderTarget().renderer().textureManager();
  if (inst.flowTexture.id == 0)
    inst.flowTexture =
        textures.createEmpty(visuals::ArtworkFlow::kWidth, visuals::ArtworkFlow::kHeight, TextureDataFormat::Rgba);
  if (inst.flowTexture.id == 0
      || !textures.updateSubImage(
          inst.flowTexture, m_flowFrame.data(), 0, 0, visuals::ArtworkFlow::kWidth, visuals::ArtworkFlow::kHeight,
          TextureDataFormat::Rgba
      )) {
    // Retry on the next update rather than caching a failed texture upload.
    inst.flowArt.clear();
    inst.flowShown = false;
    inst.flowImage->setVisible(false);
    syncFlowTimer();
    return;
  }
  inst.flowImage->setExternalTexture(inst.surface->renderTarget().renderer(), inst.flowTexture);
  inst.flowImage->setVisible(true);
  inst.flowImage->markPaintDirty();
  syncFlowTimer();
}

bool Island::animatesFlow(const Instance& inst) const {
  return inst.flowShown && !inst.panelHosted && inst.visibility > 0.001F && inst.flowTexture.id != 0;
}

void Island::syncFlowTimer() {
  const bool animate = MotionService::instance().enabled()
      && !visuals::ArtworkFlow::frozen()
      && std::ranges::any_of(m_instances, [this](const auto& inst) { return animatesFlow(*inst); });
  if (!animate)
    m_flowTimer.stop();
  else if (!m_flowTimer.active())
    m_flowTimer.startRepeating(std::chrono::milliseconds(33), [this] { tickFlow(); });
}

void Island::tickFlow() {
  syncFlowTimer();
  if (!m_flowTimer.active() || !m_renderContext)
    return;
  bool any = false;
  for (auto& ptr : m_instances) {
    auto& inst = *ptr;
    if (!animatesFlow(inst))
      continue;
    if (!any)
      m_flow.render(std::chrono::duration<float>(std::chrono::steady_clock::now() - m_flowStart).count(), m_flowFrame);
    any = true;
    m_renderContext->makeCurrent(inst.surface->renderTarget());
    (void)inst.surface->renderTarget().renderer().textureManager().updateSubImage(
        inst.flowTexture, m_flowFrame.data(), 0, 0, visuals::ArtworkFlow::kWidth, visuals::ArtworkFlow::kHeight,
        TextureDataFormat::Rgba
    );
    inst.flowImage->markPaintDirty();
    inst.surface->requestRedraw();
  }
}

void Island::releaseFlow(Instance& inst) {
  if (inst.flowTexture.id != 0 && m_renderContext && inst.surface) {
    m_renderContext->makeCurrent(inst.surface->renderTarget());
    inst.surface->renderTarget().renderer().textureManager().unload(inst.flowTexture);
  }
  inst.flowTexture = {};
  inst.flowShown = false;
  inst.flowArt.clear();
}

void Island::collapseAfterLeave(Instance& inst, std::chrono::milliseconds delay) {
  inst.leave.start(delay, [this, &inst] {
    if (inst.captureMenu || inst.captureRemaining > 0)
      return;
    // A menu opened from the Island (a tray item's) keeps it expanded until the menu closes.
    if (holdExpanded && holdExpanded()) {
      collapseAfterLeave(inst, std::chrono::milliseconds(200));
      return;
    }
    if (inst.inside)
      return;
    inst.hovered = false;
    inst.heldMedia = false;
    refresh();
  });
}

void Island::releaseKeyboard(Instance& inst) {
  inst.captureMenu = false;
  inst.keyboardMode = false;
  inst.keyboardNotification.reset();
  inst.keyboardTransferNotice.reset();
  inst.keyboardCard.reset();
  inst.hovered = false;
  inst.heldMedia = false;
  inst.suppressHover = inst.inside;
  inst.enter.stop();
  inst.leave.stop();
  inst.input.setFocus(nullptr);
  inst.surface->setKeyboardInteractivity(LayerShellKeyboard::None);
  if (m_notification
      && m_notification->urgency != Urgency::Critical
      && m_notification->timeout > 0
      && std::ranges::none_of(m_instances, [](const auto& other) { return other->inside || other->keyboardMode; }))
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  inst.signature.clear();
  refresh();
}

bool Island::openCaptureMenu(wl_output* output) {
  if (!enabled() || m_instances.empty() || !beginCapture)
    return false;
  if (cancelCapture)
    cancelCapture();
  if (closeHostedPanel)
    closeHostedPanel();
  closeCaptureMenu();
  if (!output)
    output = m_wayland->lastPointerOutput();
  const auto found = std::ranges::find_if(m_instances, [output](const auto& item) { return item->output == output; });
  auto& inst = **(found == m_instances.end() ? m_instances.begin() : found);
  inst.captureMenu = true;
  inst.keyboardMode = true;
  inst.hovered = true;
  inst.suppressHover = false;
  inst.enter.stop();
  inst.leave.stop();
  inst.input.setFocus(nullptr);
  inst.surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
  m_osd.reset();
  m_osdTimeout.stop();
  refresh();
  return true;
}

void Island::closeCaptureMenu() {
  for (auto& inst : m_instances)
    if (inst->keyboardMode || inst->captureMenu)
      releaseKeyboard(*inst);
}

void Island::setCaptureCountdown(bool recording, int remaining, const std::string& output) {
  auto found = std::ranges::find_if(m_instances, [this, &output](const auto& item) {
    const auto* monitor = m_wayland->findOutputByWl(item->output);
    return !output.empty() ? monitor && monitor->connectorName == output
                           : item->output == m_wayland->lastPointerOutput();
  });
  Instance* selected =
      m_instances.empty() ? nullptr : (found == m_instances.end() ? m_instances.front().get() : found->get());
  for (auto& item : m_instances) {
    auto& inst = *item;
    if (&inst == selected && remaining > 0) {
      inst.captureMenu = false;
      inst.captureRecording = recording;
      inst.captureRemaining = remaining;
      inst.keyboardMode = true;
      inst.enter.stop();
      inst.leave.stop();
      inst.surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
    } else if (inst.captureRemaining > 0) {
      inst.captureRemaining = 0;
      // Capture controls must disappear immediately, before the frame is taken.
      inst.skipCrossfade = true;
      releaseKeyboard(inst);
    }
  }
  refresh();
}

bool Island::focusKeyboard() {
  if (!enabled() || m_instances.empty())
    return false;
  for (auto& inst : m_instances)
    if (inst->captureMenu || inst->captureRemaining > 0) {
      inst->keyboardMode = true;
      inst->surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
      refresh();
      return true;
    }
  m_transferActivation.stop();
  if (closeHostedPanel)
    closeHostedPanel();
  for (auto& inst : m_instances)
    if (inst->keyboardMode)
      releaseKeyboard(*inst);
  auto found = std::ranges::find_if(m_instances, [this](const auto& inst) {
    return inst->output == m_wayland->lastPointerOutput();
  });
  auto& inst = **(found == m_instances.end() ? m_instances.begin() : found);
  const auto timers = countdowns();
  if (!m_notification
      && !(m_transferNotice && transferApp(m_transferNotice->source))
      && !cardActionKey(inst).has_value()
      && (!m_mpris || !m_mpris->activePlayer())
      && progressActivities().empty()
      && (!inst.config.hoverShowDownloads || m_recentTransfers.empty())
      && !awakeRemaining()
      && !ScreenRecorder::instance().active()
      && !inst.captureMenu
      && inst.captureRemaining <= 0
      && m_screenSessions.sessions().empty()
      && m_cameraSessions.sessions().empty()
      && std::ranges::none_of(
          privacy(), [](const auto& activity) { return activity.kind == PrivacyCaptureKind::Microphone; }
      )
      && std::ranges::none_of(timers, [](const auto& timer) { return timer.active; })) {
    if (!openPanel)
      return false;
    openPanel(inst.output, "calendar");
    return true;
  }
  m_osd.reset();
  m_osdTimeout.stop();
  inst.keyboardMode = true;
  inst.keyboardNotification = m_notification ? std::optional{m_notification->id} : std::nullopt;
  inst.keyboardTransferNotice = !m_notification && m_transferNotice && transferApp(m_transferNotice->source)
      ? std::optional{m_transferNotice->serial}
      : std::nullopt;
  inst.keyboardCard = !m_notification && !m_transferNotice ? cardActionKey(inst) : std::nullopt;
  inst.enter.stop();
  inst.leave.stop();
  inst.input.setFocus(nullptr);
  if (m_notification && m_notification->timeout > 0)
    m_notifications->pauseExpiry(m_notification->id);
  inst.surface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
  inst.signature.clear();
  refresh();
  return true;
}

bool Island::onKeyboardEvent(const KeyboardEvent& event) {
  if (!m_wayland)
    return false;
  for (auto& item : m_instances) {
    auto& inst = *item;
    if (!inst.keyboardMode || inst.panelHosted || m_wayland->lastKeyboardSurface() != inst.surface->wlSurface())
      continue;
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      const auto notification = inst.keyboardNotification;
      releaseKeyboard(inst);
      if (notification && m_notification && m_notification->id == notification)
        dismissNotification();
      return true;
    }
    if (event.pressed
        && !event.preedit
        && event.modifiers == 0
        && inst.captureMenu
        && inst.previousView == island::View::CaptureMenu
        && (KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym))
        && inst.input.focusedArea()) {
      const std::string key(inst.input.focusedArea()->tabFocusKey());
      const auto cycle = [&](std::vector<std::string> keys) -> std::string {
        const auto found = std::ranges::find(keys, key);
        if (found == keys.end())
          return {};
        const auto index = static_cast<std::size_t>(found - keys.begin());
        return keys[(index + (KeySymbol::isLeft(event.sym) ? keys.size() - 1 : 1)) % keys.size()];
      };
      auto next = cycle({"capture-menu-screenshot", "capture-menu-record"});
      if (!next.empty()) {
        inst.captureOptions.recording = next == "capture-menu-record";
        if (inst.captureOptions.recording && inst.captureOptions.target == capture::Target::Window)
          inst.captureOptions.target = capture::Target::Region;
      } else if (!(next = cycle(
                       inst.captureOptions.recording
                           ? std::vector<std::string>{"capture-menu-region", "capture-menu-monitor"}
                           : std::vector<
                                 std::string>{"capture-menu-region", "capture-menu-window", "capture-menu-monitor"}
                   ))
                      .empty()) {
        inst.captureOptions.target = next == "capture-menu-window" ? capture::Target::Window
            : next == "capture-menu-monitor"                       ? capture::Target::Monitor
                                                                   : capture::Target::Region;
      } else if (!(next = cycle(
                       {"capture-menu-delay-0", "capture-menu-delay-3", "capture-menu-delay-5", "capture-menu-delay-10"}
                   ))
                      .empty()) {
        inst.captureOptions.delaySeconds = next.ends_with("-10") ? 10
            : next.ends_with("-5")                               ? 5
            : next.ends_with("-3")                               ? 3
                                                                 : 0;
      } else if (
          inst.captureOptions.recording
          && !(next = cycle({"capture-menu-audio-off", "capture-menu-audio-desktop", "capture-menu-audio-microphone"}))
                  .empty()
      ) {
        inst.captureOptions.audio = next.ends_with("-off") ? capture::RecordingAudio::Off
            : next.ends_with("-desktop")                   ? capture::RecordingAudio::Desktop
                                                           : capture::RecordingAudio::Microphone;
      }
      if (!next.empty()) {
        inst.captureError.clear();
        refresh();
        inst.input.restoreTabFocus({.key = std::move(next)});
        inst.surface->requestRedraw();
        return true;
      }
    }
    if (inst.content && inst.content->opacity() > 0.1F)
      inst.input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    inst.surface->requestRedraw();
    return true;
  }
  return false;
}

bool Island::onPointerEvent(const PointerEvent& event) {
  for (auto& ptr : m_instances) {
    auto& inst = *ptr;
    if (!inst.panelHosted && inst.hoverWidgets && inst.hoverWidgets->onPointerEvent(event))
      return true;
    if (inst.panelHosted || event.surface != inst.surface->wlSurface())
      continue;
    if (event.type == PointerEvent::Type::Enter) {
      inst.inside = true;
      inst.compactActivity.pause(island::CompactActivity::Clock::now());
      inst.activityTimeout.stop();
      updateVisibility(inst);
      inst.input.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      inst.leave.stop();
      if (m_notification && m_notification->timeout > 0)
        m_notifications->pauseExpiry(m_notification->id);
      if (!inst.suppressHover)
        inst.enter.start(std::chrono::milliseconds(inst.config.hoverOpenDelayMs), [this, &inst] {
          if (inst.inside && !inst.badgeHovered && !inst.splitHovered() && !cardActionKey(inst).has_value()) {
            inst.hovered = true;
            refresh();
          }
        });
    } else if (event.type == PointerEvent::Type::Leave) {
      inst.input.pointerLeave();
      inst.seeking = false;
      inst.activeSeek = {};
      inst.inside = false;
      inst.suppressHover = false;
      inst.enter.stop();
      inst.pressedAction.clear();
      if (m_notification
          && m_notification->urgency != Urgency::Critical
          && m_notification->timeout > 0
          && std::ranges::none_of(m_instances, [](const auto& other) { return other->inside || other->keyboardMode; }))
        m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
      collapseAfterLeave(inst, std::chrono::milliseconds(inst.config.hoverCloseDelayMs));
      refresh();
    } else if (event.type == PointerEvent::Type::Motion) {
      if (!inst.seeking) {
        inst.input.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      const float x = (static_cast<float>(event.sx) - inst.background->x() - inst.content->x()) / inst.scale;
      inst.seekFraction = std::clamp((x - inst.seekX) / inst.seekWidth, 0.0F, 1.0F);
      if (inst.seekProgress)
        inst.seekProgress->setProgress(inst.seekFraction);
      inst.surface->requestUpdate();
    } else if (event.type == PointerEvent::Type::Axis) {
      inst.input.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines, event.axisGestureSerial
      );
    } else if (event.type == PointerEvent::Type::Button && event.button != BTN_LEFT && inst.content) {
      // Other buttons reach controls that accept them (tray menus, widget gestures); the
      // Island's own actions are left-click only.
      if (!inst.seeking && inst.content->opacity() > 0.1F)
        (void)inst.input.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
            event.time, event.touch
        );
    } else if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && inst.content) {
      // A newly rebuilt card may still be concealed during the size transition.
      if (inst.content->opacity() <= 0.1F) {
        inst.pressedAction.clear();
        inst.input.cancelPointerCapture();
        return true;
      }
      if (!inst.seeking
          && inst.input.pointerButton(
              static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
              event.time, event.touch
          )) {
        // A click can lend this surface to a panel. Drop any hover restored by
        // the button dispatcher after its callback, including pending tooltips.
        if (inst.panelHosted) {
          inst.input.pointerLeave();
          TooltipManager::instance().forceDestroy();
        }
        return true;
      }
      const float x = (static_cast<float>(event.sx) - inst.background->x() - inst.content->x()) / inst.scale;
      const float y = (static_cast<float>(event.sy) - inst.background->y()) / inst.scale;
      if (event.pressed
          && inst.seek
          && y >= inst.seekY
          && y < inst.seekY + 22
          && x >= inst.seekX
          && x <= inst.seekX + inst.seekWidth) {
        inst.seeking = true;
        inst.seekTrackSignature = m_trackSignature;
        inst.activeSeek = inst.seek;
        inst.seekFraction = std::clamp((x - inst.seekX) / inst.seekWidth, 0.0F, 1.0F);
        inst.surface->requestUpdate();
        return true;
      }
      if (!event.pressed && inst.seeking) {
        inst.seeking = false;
        auto seek = std::move(inst.activeSeek);
        if (seek)
          seek(inst.seekFraction);
        inst.surface->requestUpdate();
        return true;
      }
      std::string hit;
      std::function<void()> cb;
      for (const auto& action : inst.actions) {
        if (x >= action.x && x < action.x + action.width && y >= action.y && y < action.y + action.height) {
          hit = action.id;
          cb = action.invoke;
          break;
        }
      }
      if (event.pressed)
        inst.pressedAction = hit;
      else {
        const bool activate = !hit.empty() && hit == inst.pressedAction;
        inst.pressedAction.clear();
        if (activate && cb)
          cb();
      }
    }
    return true;
  }
  return false;
}

std::optional<IslandPanelSurface>
Island::acquirePanelSurface(wl_output* output, bool exactOutput, std::string_view barName) {
  if (!enabled() || m_instances.empty())
    return std::nullopt;
  const bool legacy = m_instances.front()->barConfig.name == "__legacy_island";
  auto it = std::ranges::find_if(m_instances, [output, barName, legacy](const auto& inst) {
    return inst->output == output && (legacy || barName.empty() || inst->barConfig.name == barName);
  });
  if ((exactOutput || !legacy) && it == m_instances.end())
    return std::nullopt;
  // Shell shortcuts on another monitor still open the configured island.
  auto& inst = **(it == m_instances.end() ? m_instances.begin() : it);
  if (inst.panelHosted)
    return std::nullopt;
  if (inst.captureRemaining > 0 && cancelCapture)
    cancelCapture();
  if (inst.keyboardMode)
    releaseKeyboard(inst);
  inst.panelHosted = true;
  for (auto* meter : inst.microphoneMeters)
    meter->stop();
  inst.microphoneMeters.clear();
  syncFlowTimer();
  // Panels can be as tall as the output; the Island shrinks the surface again once it settles.
  inst.surfaceHeight = static_cast<std::uint32_t>(inst.outputHeight);
  inst.surface->requestSize(inst.surfaceWidth, inst.surfaceHeight);
  // The panel opens from the capsule alone; the split bubbles bud out again when it closes.
  for (auto& split : inst.splits) {
    inst.animations.cancel(split.morph);
    inst.animations.cancel(split.fade);
    split.fade = 0;
    split.contentFade = 1;
    if (split.outgoing && split.area)
      (void)split.area->removeChild(split.outgoing);
    split.outgoing = nullptr;
    split.target.clear();
    split.pressedTarget.clear();
    split.activity = island::Activity::None;
    split.reveal = 0;
    split.morph = 0;
    split.hovered = false;
    if (split.bubble)
      split.bubble->setVisible(false);
  }
  inst.compactActivity.pause(island::CompactActivity::Clock::now());
  inst.activityTimeout.stop();
  inst.animations.cancel(inst.hideAnimation);
  inst.visibility = 1;
  inst.wantsVisible = true;
  if (inst.visualizer) {
    inst.content->removeChild(inst.visualizer);
    inst.visualizer = nullptr;
  }
  inst.input.pointerLeave();
  TooltipManager::instance().forceDestroy();
  inst.enter.stop();
  inst.leave.stop();
  // The panel draws its own glow; restart the capsule's pulse when it returns.
  if (inst.captureGlow)
    inst.captureGlow->update(false, island::CaptureGlow::sharingColor());
  inst.animations.cancelAll();
  // Finish any view crossfade the cancel cut short, so the Island comes back fully drawn.
  clearCrossfade(inst);
  inst.seeking = false;
  inst.activeSeek = {};
  inst.pressedAction.clear();
  inst.inside = false;
  inst.hovered = false;
  inst.heldMedia = false;
  inst.suppressHover = true;
  if (m_notification && m_notification->urgency != Urgency::Critical && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  return IslandPanelSurface{
      inst.surface.get(),
      inst.output,
      inst.width * inst.scale,
      inst.height * inst.scale,
      inst.scale,
      inst.flowShown ? inst.flowTexture : TextureHandle{},
      inst.config.appearance == IslandAppearance::Cupertino,
      inst.config.compactLayout
  };
}

Color Island::capsuleColor() const { return resolveColorSpec(islandRole(ColorRole::Surface)); }

float Island::capsuleOpacity() const {
  const auto hosted = std::ranges::find_if(m_instances, [](const auto& inst) { return inst->panelHosted; });
  if (hosted != m_instances.end())
    return glassOpacity((*hosted)->config);
  return glassOpacity(m_instances.empty() ? m_config->config().island : m_instances.front()->config);
}

void Island::updateGlass(Instance& inst, float x, float y, float radius) {
  const float glass = glassOpacity(inst.config);
  inst.background->setFill(islandRole(ColorRole::Surface, glass));
  for (const auto& split : inst.splits)
    if (split.bubble)
      split.bubble->setFill(islandRole(ColorRole::Surface, glass));
  // The compositor blurs (and hyprglass glasses) this shape behind the capsule and the split
  // bubble; see Surface::setBlurRegionRectLimit for how hyprglass gets smooth edges from it.
  std::vector<InputRect> rects;
  if (glass < 1.0F && inst.visibility > 0.01F) {
    rects = Surface::tessellateRoundedRect(
        static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
        static_cast<int>(std::lround(inst.width * inst.scale)), static_cast<int>(std::lround(inst.height * inst.scale)),
        radius
    );
    for (const auto& split : inst.splits) {
      if (!split.bubble || !split.bubble->visible())
        continue;
      const float d = split.bubble->width();
      auto more = Surface::tessellateRoundedRect(
          static_cast<int>(std::lround(split.bubble->x())), static_cast<int>(std::lround(split.bubble->y())),
          static_cast<int>(std::lround(d)), static_cast<int>(std::lround(d)), d / 2
      );
      rects.insert(rects.end(), more.begin(), more.end());
    }
  }
  std::vector<std::array<int, 4>> key;
  key.reserve(rects.size());
  for (const auto& r : rects)
    key.push_back({r.x, r.y, r.width, r.height});
  if (key == inst.blurRegion)
    return;
  inst.blurRegion = std::move(key);
  inst.surface->setBlurRegion(rects);
  inst.surface->requestRedraw();
}

island::Size Island::panelReturnSize() const {
  const auto hosted = std::ranges::find_if(m_instances, [](const auto& inst) { return inst->panelHosted; });
  const auto& cfg = hosted != m_instances.end() ? (*hosted)->config
      : !m_instances.empty()                    ? m_instances.front()->config
                                                : m_config->config().island;
  wl_output* output = hosted != m_instances.end() ? (*hosted)->output
      : !m_instances.empty()                      ? m_instances.front()->output
                                                  : nullptr;
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const bool mediaActive =
      player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds);
  const auto timers = countdowns();
  const island::Activities available{
      mediaActive, !progressActivities().empty(),
      std::ranges::any_of(timers, [](const auto& timer) { return timer.active; }), false, awakeRemaining().has_value()
  };
  const auto* instance = hosted != m_instances.end() ? hosted->get()
      : !m_instances.empty()                         ? m_instances.front().get()
                                                     : nullptr;
  const bool announcing = player && (!player->title.empty() || !player->artists.empty()) && trackPreview(cfg, output);
  const auto compact = announcing ? island::Activity::Media
                                  : island::preferredActivity(
                                        available, island::activityOrder(cfg.activityPriority),
                                        instance ? instance->compactActivity.selected() : island::Activity::None
                                    );
  const bool recording = ScreenRecorder::instance().active();
  const auto view = island::view(
      m_notification.has_value(), m_osd.has_value() && !(instance && connectionAudioOsd(*instance)), false, mediaActive,
      false, available.downloads, available.timers, true, true, island::Activity::None, compact,
      m_transferNotice.has_value(), instance && instance->connection.has_value(),
      instance && instance->network.has_value(), false, available.awake, !m_screenSessions.sessions().empty(), recording
  );
  auto size = island::size(
      view, cfg.height, cfg.clockSize, cfg.clockSeconds, cfg.calendarLabels != IslandCalendarLabels::Initials,
      cfg.mediaArtworkSize, announcing
  );
  const auto batteryList = recording ? std::vector<island::Battery>{} : batteries(cfg, output);
  const bool unread = !recording
      && m_notifications
      && std::ranges::any_of(m_notifications->history(), [](const auto& item) { return !item.seen; });
  const auto privacyList = island::showsStatusIcons(view) ? privacy() : std::vector<island::PrivacyActivity>{};
  // The unread bell shares the privacy slot when both are active.
  size.width = island::batteryWidth(
      size.width, view, !batteryList.empty() && batteryList.front().compact(), unread && privacyList.empty()
  );
  if (!privacyList.empty()) {
    if (view == island::View::Rest
        || view == island::View::Activity
        || view == island::View::DownloadActivity
        || view == island::View::AwakeActivity
        || view == island::View::RecordingActivity
        || view == island::View::TimerActivity)
      size.width += 2 * (24.0F + 8.0F); // One indicator slot; see PrivacyRotation.
    else if (view == island::View::Notification)
      size.height += 32;
  }
  return size;
}

void Island::releasePanelSurface(wl_output* output, float width, float height) {
  for (auto& ptr : m_instances) {
    auto& inst = *ptr;
    if (inst.output != output || !inst.panelHosted)
      continue;
    inst.panelHosted = false;
    inst.skipCrossfade = true;
    // Drop an OSD queued while the panel was open, and the panel's own last changes (a
    // debounced slider commit can land just after it closes).
    m_osd.reset();
    m_osdTimeout.stop();
    m_osdQuietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    inst.inputRegion.reset(); // The hosted panel installed its own input region.
    inst.suppressHover = false;
    inst.width = inst.targetWidth = width / inst.scale;
    inst.height = inst.targetHeight = height / inst.scale;
    inst.widthVelocity = inst.heightVelocity = 0;
    inst.signature.clear();
    inst.surface->setSceneRoot(inst.root.get());
    inst.surface->setAnimationManager(&inst.animations);
    inst.surface->setFrameTickCallback([p = ptr.get()](float dt) {
      if (!p->panelHosted && p->visualizer)
        p->visualizer->onFrameTick(dt);
      if (!p->panelHosted && p->hoverWidgets)
        p->hoverWidgets->tickWidgets(dt);
    });
    inst.surface->setConfigureCallback([p = ptr.get()](std::uint32_t, std::uint32_t) {
      p->signature.clear();
      p->surface->requestUpdate();
    });
    inst.surface->setPrepareFrameCallback([this, p = ptr.get()](bool, bool) { prepare(*p); });
    inst.surface->setKeyboardInteractivity(LayerShellKeyboard::None);
    inst.surface->setLayer(inst.barConfig.layer == "overlay" ? LayerShellLayer::Overlay : LayerShellLayer::Top);
    inst.surface->setBlurRegion({});
    inst.blurRegion.clear(); // the hosted panel sent its own; geometry() resends the capsule's
    updateVisibility(inst);
    inst.surface->requestUpdate();
  }
}
