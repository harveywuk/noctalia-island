#include "shell/island/island.h"

#include "calendar/calendar_service.h"
#include "capture/screen_recorder.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/downloads/download_progress_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
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
  class IslandAudioVisualizer : public AudioVisualizer {
  public:
    IslandAudioVisualizer(PipeWireSpectrum* spectrum, LayerSurface& surface)
        : m_spectrum(spectrum), m_surface(surface) {
      setCentered(true);
      setMirrored(false);
      // Quiet passages rest as dots, as Apple's Now Playing waveform does, not a dashed line.
      setRestAsDots(true);
      setValues(std::vector<float>(5, 0.0F));
      if (m_spectrum) {
        m_listener = m_spectrum->addChangeListener(5, [this] { m_surface.requestFrameTick(); });
      }
      m_surface.requestFrameTick();
    }

    ~IslandAudioVisualizer() override {
      if (m_spectrum && m_listener)
        m_spectrum->removeChangeListener(m_listener);
    }

    void onFrameTick(float deltaMs) {
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
  };
} // namespace

struct Island::Instance {
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
  Node* content = nullptr;
  // View crossfades: the outgoing content fading out, and the incoming content's fade-in factor.
  Node* outgoing = nullptr;
  float contentFade = 1.0F;
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
  // Split Island: other running activities in round bubbles beside the capsule, as on iPhone.
  // The first sits behind the capsule and slides out from under its right end; the second sits
  // behind the first and slides out from under that, for a third concurrent activity.
  struct SplitBubble {
    Box* bubble = nullptr;
    InputArea* area = nullptr;
    Node* content = nullptr;
    island::Activity activity = island::Activity::None;
    std::string signature;
    std::function<void(float)> progress;
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
  island::ActivitySelection activities;
  island::CompactActivity compactActivity;
  Timer activityTimeout;
  std::optional<std::uint32_t> keyboardNotification;
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
    std::string desktopId;
    Label* percentage;
    ProgressBar* progress;
  };
  std::vector<DownloadUi> downloadUi;
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
  m_platform = &services.platform;
  m_widgetFactory = std::make_unique<WidgetFactory>(services);
  m_widgetActions.setIpcService(ipc);
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
  for (const auto& activity : m_scriptActivities)
    result.push_back({
        .desktopId = "script:" + activity.id,
        .name = activity.title,
        .progress = activity.progress.value_or(0.0),
        .determinate = activity.progress.has_value(),
        .phase = "working",
        .icon = activity.icon.empty() ? "terminal-2" : activity.icon,
    });
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
  it->updated = std::chrono::steady_clock::now();
  expireScriptActivities();
  refresh();
  return true;
}

bool Island::updateScriptActivity(
    const std::string& id, std::optional<std::optional<double>> progress, const std::string& title,
    const std::string& icon
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
  it->updated = std::chrono::steady_clock::now();
  expireScriptActivities();
  refresh();
  return true;
}

bool Island::endScriptActivity(const std::string& id) {
  if (std::erase_if(m_scriptActivities, [&id](const auto& activity) { return activity.id == id; }) == 0)
    return false;
  expireScriptActivities();
  refresh();
  return true;
}

void Island::timerCommand(const island::Countdown& timer, const std::string& command) {
  if (!scripting::PluginRegistry::instance().hasEntry(timer.panel))
    return;
  scripting::PluginStateStore::instance().set(timer.plugin, timer.commandKey(), nlohmann::json(command).dump());
  refresh();
}

void Island::destroySurfaces() {
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
  if (enabled()) {
    onOutputChange();
    m_tick.startRepeating(1s, [this] { refresh(); });
  } else {
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

void Island::refresh() {
  if (!enabled())
    return;
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
  m_batteryConnections->reconcileOutputs(outputs, target.output);
  hideDndSuppressed();
  updateNotificationPreview();
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const std::string track = player ? player->busName + logicalTrackSignature(*player) : "";
  m_mediaActivity.update(track, player ? player->playbackStatus : "", now, target.output);
  m_mediaActivity.reconcileOutputs(outputs, target.output);
  m_trackSignature = track;
  std::optional<TimePoint> mediaExpiry, batteryExpiry;
  for (const auto& inst : m_instances) {
    const auto& cfg = inst->config;
    if (const auto expiry = m_mediaActivity.nextExpiry(now, cfg.trackPreviewSeconds, cfg.pausedMediaSeconds);
        expiry && (!mediaExpiry || *expiry < *mediaExpiry))
      mediaExpiry = expiry;
    if (const auto expiry = m_batteryConnections->nextExpiry(now, cfg.bluetoothPreviewSeconds);
        expiry && (!batteryExpiry || *expiry < *batteryExpiry))
      batteryExpiry = expiry;
  }
  if (mediaExpiry)
    m_mediaTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*mediaExpiry - now), [this] { refresh(); });
  else
    m_mediaTimeout.stop();
  if (batteryExpiry)
    m_batteryTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*batteryExpiry - now), [this] { refresh(); });
  else
    m_batteryTimeout.stop();
  const auto timers = countdowns();
  const bool timerActive = std::ranges::any_of(timers, [](const auto& timer) { return timer.active; });
  const bool downloadActive = !progressActivities().empty();
  for (auto& inst : m_instances) {
    updateVisibility(*inst);
    const auto& cfg = inst->config;
    inst->compactActivity.update(
        {player && m_mediaActivity.compact(now, cfg.pausedMediaSeconds), downloadActive, timerActive},
        // Split activities are all in view, so they never cycle.
        cfg.activityPriority, cfg.cycleActivities && !cfg.splitActivities, cfg.activityCycleSeconds,
        inst->inside
            || inst->hovered
            || inst->keyboardMode
            || inst->panelHosted
            || !inst->wantsVisible
            || m_notification
            || m_osd
            || ScreenRecorder::instance().active(),
        now
    );
    if (const auto expiry = inst->compactActivity.nextExpiry())
      inst->activityTimeout.start(std::chrono::ceil<std::chrono::milliseconds>(*expiry - now), [this] { refresh(); });
    else
      inst->activityTimeout.stop();
    if (!inst->panelHosted) {
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
        && ((event == NotificationEvent::Closed && inst->keyboardNotification == n.id)
            || (event == NotificationEvent::Added
                && inst->keyboardNotification != n.id
                && !(m_notifications->doNotDisturb() && n.dndPolicy == NotificationDndPolicy::Respect))))
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
  if (m_notifications->doNotDisturb() && n.dndPolicy == NotificationDndPolicy::Respect)
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
    if (m_notifications->doNotDisturb() && queued.dndPolicy == NotificationDndPolicy::Respect) {
      if (queued.timeout > 0)
        m_notifications->resumeExpiry(queued.id, queued.timeout);
      return true;
    }
    return false;
  });
  if (m_notification
      && m_notifications
      && m_notifications->doNotDisturb()
      && m_notification->dndPolicy == NotificationDndPolicy::Respect) {
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
      split.area->setHitTestVisible(reveal > 0.5F);
    }
    if (split.content) {
      // The content is laid out for the full bubble; keep it centred while the bubble grows.
      split.content->setPosition((diameter - bubble) * s / 2, (diameter - bubble) * s / 2);
      split.content->setOpacity(std::clamp((reveal - 0.4F) / 0.6F, 0.0F, 1.0F));
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
    const float room = std::clamp(1 - shortfall / 10, 0.0F, 1.0F) * std::clamp(1 - excess / 55, 0.0F, 1.0F);
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
  const std::string announcement = player && trackPreview(cfg, inst.output) ? player->title : "";
  auto downloads = progressActivities();
  // A bubble click put this job in the capsule; it stays there while it runs.
  if (const auto lead = std::ranges::find(downloads, inst.splitLead, &DownloadProgress::desktopId);
      lead != downloads.end())
    std::rotate(downloads.begin(), lead, lead + 1);
  const auto timers = countdowns();
  const bool timerActive = !timers.empty() && timers.front().active;
  const bool recording = ScreenRecorder::instance().active();
  if (inst.keyboardMode
      && (recording
          || (inst.keyboardNotification && (!m_notification || m_notification->id != inst.keyboardNotification))
          || (!inst.keyboardNotification && !player && downloads.empty() && !timerActive)))
    releaseKeyboard(inst);
  const bool expansionRequested = inst.hovered || inst.keyboardMode;
  if (player && expansionRequested && (playing || player->playbackStatus == "Paused" || inst.keyboardMode))
    inst.heldMedia = true;
  if (!player || !expansionRequested)
    inst.heldMedia = false;
  const island::Activities availableActivities{
      cfg.hoverShowMedia && player && (playing || inst.heldMedia), cfg.hoverShowDownloads && !downloads.empty(),
      cfg.hoverShowTimers && timerActive
  };
  inst.activities.update(expansionRequested && !recording, availableActivities);
  // Keep the existing calendar/timer layout for a lone timer, until switching is useful.
  const auto selected = inst.activities.selected == island::Activity::Timers && !inst.activities.switching
      ? island::Activity::None
      : inst.activities.selected;
  const auto view = recording
      ? island::View::Rest
      : island::view(
            m_notification.has_value(), m_osd.has_value() && !inst.keyboardMode, expansionRequested,
            player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds),
            inst.heldMedia, !downloads.empty(), timerActive, cfg.hoverShowMedia, cfg.hoverShowDownloads, selected,
            inst.compactActivity.selected()
        );
  const bool compactView = view == island::View::Rest
      || view == island::View::Activity
      || view == island::View::DownloadActivity
      || view == island::View::TimerActivity;
  const bool expandedView = view == island::View::Calendar
      || view == island::View::Media
      || view == island::View::Downloads
      || view == island::View::Timers;
  const bool showSwitcher = expandedView && inst.activities.switching && availableActivities.count() > 0;
  // Split Island: with two activities running, the compact capsule shows one and a bubble beside
  // it the other, rather than the activity order hiding the second.
  const auto primaryActivity = view == island::View::Activity ? island::Activity::Media
      : view == island::View::DownloadActivity                ? island::Activity::Downloads
      : view == island::View::TimerActivity                   ? island::Activity::Timers
                                                              : island::Activity::None;
  const auto otherActivity = cfg.splitActivities && !recording && primaryActivity != island::Activity::None
      ? island::secondaryActivity(
            {player && m_mediaActivity.compact(island::MediaActivity::Clock::now(), cfg.pausedMediaSeconds),
             !downloads.empty(), timerActive},
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
      !downloads.empty(), timerActive
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
  // The media card also keeps a compact tray row below its playback controls.
  // Temporary OSDs keep their own content; capture resumes in the following view.
  const bool showExtras = !gCupertino || view == island::View::Calendar;
  const auto batteryList =
      !recording && (compactView || expandedView) ? batteries(cfg, inst.output) : std::vector<island::Battery>{};
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
          || view == island::View::Timers
          || view == island::View::TimerActivity);
  constexpr float badgeWidth = 24.0F;
  auto privacyList = privacy();
  const bool capturing = !privacyList.empty();
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
  const auto time = recording ? ScreenRecorder::instance().label() : localTime(cfg.clockSeconds ? "%H:%M:%S" : "%H:%M");
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
  std::string actionSignature;
  if (m_notification)
    for (const auto& part : m_notification->actions)
      actionSignature += part + "\n";
  // Hidden events must not reconstruct the visible card. Besides avoiding needless
  // texture work, this preserves a notification while an OSD waits underneath it.
  std::string signature = std::to_string(static_cast<int>(view));
  switch (view) {
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
    signature += time + announcement + artPath + (playing ? "playing" : "paused");
    break;
  case island::View::Calendar:
    signature += time + date;
    break;
  case island::View::Rest:
    signature += recording ? "recording" : time;
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
          "|{}|{}|{}|{}|{}|{}", download.desktopId, download.name,
          view == island::View::Downloads ? 0L : std::lround(download.progress * 100), download.determinate,
          download.phase, download.icon
      );
    break;
  }
  if (view != island::View::Notification && view != island::View::Osd)
    signature += std::format("|unread:{}|count:{}", showUnread, showUnread && expandedView ? unreadCount : 0);
  signature += std::format("|keyboard:{}", inst.keyboardMode);
  if (expandedView)
    signature += std::format(
        "|switcher:{}:{}:{}:{}", showSwitcher, availableActivities.media, availableActivities.downloads,
        availableActivities.timers
    );
  if (expandedView || view == island::View::TimerActivity)
    for (const auto& timer : timers)
      signature += std::format(
          "|timer:{}|{}|{}|{}|{}|{}|{}|{}|{}", timer.plugin, countdownTitle(timer), timer.running, timer.active,
          timer.finished, timer.duration, timer.session, timer.url, timer.event && timer.remaining <= 0
      );
  for (const auto& activity : privacyList)
    signature += std::format("|privacy:{}:{}", static_cast<int>(activity.kind), activity.appNames());
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
    inst.hoverWidgets->updateWidgets(renderer, inst.hoverWidgets->width());
    if (oldHeight != inst.hoverWidgets->height())
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
  const bool outlineBattery = cfg.outerProgressRing
      && !recording
      && !outlineTimer
      && !outlineDownload
      && (showBattery || (expandedView && showExtras && cfg.hoverShowBatteries && !batteryList.empty()));
  const auto updateOutline = [&] {
    if (!inst.progressOutline)
      return;
    std::optional<float> fraction;
    ColorSpec fill = islandRole(ColorRole::Primary);
    bool charging = false;
    if (outlineTimer) {
      fraction = timers.front().fraction();
      fill = islandTint(countdownTint(timers.front()), ColorRole::Primary);
    } else if (outlineDownload) {
      fill = islandTint(kAppleBlue, ColorRole::Primary);
      // Each download gets equal weight; one unknown total makes the group indeterminate. When
      // the jobs split between capsule and bubble, the capsule's edge shows only its own.
      if (std::ranges::all_of(capsuleDownloads, [](const auto& d) { return d.determinate; })) {
        float total = 0;
        for (const auto& d : capsuleDownloads)
          total += static_cast<float>(d.progress);
        fraction = total / static_cast<float>(capsuleDownloads.size());
      }
    } else if (outlineBattery) {
      fraction = static_cast<float>(batteryList.front().percentage / 100.0);
      fill = batteryList.front().low ? islandRole(ColorRole::Error) : islandTint(kAppleGreen, ColorRole::Primary);
      charging = batteryList.front().charging();
    }
    inst.progressOutline->update(
        outlineTimer || outlineDownload || outlineBattery, fraction, fill, charging,
        islandRole(ColorRole::OnSurface, 0.16F)
    );
  };
  updateOutline();
  // Capture (microphone, camera, screen) and screen recording pulse red around the Island.
  // A critical notification pulses around the Island like capture does, so it reads as urgent.
  const bool criticalShown =
      view == island::View::Notification && m_notification && m_notification->urgency == Urgency::Critical;
  const auto updateCaptureGlow = [&] {
    if (inst.captureGlow)
      inst.captureGlow->update(capturing || recording || criticalShown, islandRole(ColorRole::Error));
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
    else if (splitActivity == island::Activity::Downloads && std::ranges::all_of(bubbleDownloads, [](const auto& item) {
               return item.determinate;
             })) {
      float total = 0;
      for (const auto& item : bubbleDownloads)
        total += static_cast<float>(item.progress);
      fraction = total / static_cast<float>(bubbleDownloads.size());
    }
    // A lone job in the bubble shows its own symbol; a group shows the download arrow.
    const std::string bubbleIcon = bubbleDownloads.size() == 1 && !bubbleDownloads.front().icon.empty()
        ? bubbleDownloads.front().icon
        : "download";
    if (index == 0) {
      inst.splitLane = laneSplit;
      inst.splitNext = laneSplit ? bubbleDownloads.front().desktopId : "";
    }
    // A retracting bubble keeps its last content until it is tucked away.
    if (splitActivity != island::Activity::None) {
      std::string bubbleSignature = std::format("{}|{}|{}|{}", static_cast<int>(splitActivity), d, s, gCupertino);
      if (splitActivity == island::Activity::Media)
        bubbleSignature += "|" + artPath;
      else if (splitActivity == island::Activity::Timers)
        bubbleSignature += "|" + timers.front().plugin + "|" + timers.front().icon;
      else
        bubbleSignature += std::format("|{}|{}|{}", fraction.has_value(), bubbleIcon, laneSplit ? inst.splitNext : "");
      if (bubbleSignature != split.signature) {
        m_renderContext->makeCurrent(inst.surface->renderTarget());
        split.signature = bubbleSignature;
        split.progress = {};
        if (split.content)
          (void)split.area->removeChild(split.content);
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
        if (splitActivity == island::Activity::Timers || splitActivity == island::Activity::Downloads) {
          // A progress ring in the activity's colour round the bubble's own rim, the way the
          // capsule's progress traces its edge, with the activity's symbol in the middle.
          const bool timer = splitActivity == island::Activity::Timers;
          const auto tint =
              timer ? islandTint(kAppleOrange, ColorRole::Primary) : islandTint(kAppleBlue, ColorRole::Primary);
          auto ring = std::make_unique<DownloadRing>(d * s, 3.0F * s, fraction, tint);
          auto* ringPtr = ring.get();
          centred(std::move(ring));
          if (fraction)
            split.progress = [ringPtr](float value) { ringPtr->setProgress(value); };
          symbol(timer ? timers.front().icon : bubbleIcon, tint, 0.34F);
        }
        split.content = split.area->addChild(std::move(content));
        split.area->setTooltip(
            laneSplit && bubbleDownloads.size() == 1
                ? bubbleDownloads.front().name
                : i18n::tr(
                      splitActivity == island::Activity::Media        ? "island.split.media"
                          : splitActivity == island::Activity::Timers ? "island.split.timers"
                                                                      : "island.split.downloads"
                  )
        );
      }
      if (split.progress && fraction)
        split.progress(*fraction);
    }
    const bool shown = splitActivity != island::Activity::None;
    const bool wasShown = split.activity != island::Activity::None;
    split.activity = splitActivity;
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
  if (signature == inst.signature && inst.root) {
    if ((recording && inst.recordingLabel)
        || !inst.timerUi.empty()
        || !inst.downloadUi.empty()
        || (view == island::View::Media && player))
      m_renderContext->makeCurrent(inst.surface->renderTarget());
    geometry(inst);
    // Timer ticks must not rebuild the stop action between pointer press and release.
    if (recording && inst.recordingLabel) {
      inst.recordingLabel->setText(time);
      inst.recordingLabel->measure(renderer);
      const float clockY =
          (cfg.height * inst.scale - inst.recordingLabel->height()) / 2.0F + cfg.clockOffset * inst.scale;
      inst.recordingLabel->setPosition(
          inst.recordingLabel->x(),
          std::clamp(clockY, 0.0F, std::max(0.0F, cfg.height * inst.scale - inst.recordingLabel->height()))
      );
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
      const auto download = std::ranges::find(downloads, ui.desktopId, &DownloadProgress::desktopId);
      if (download == downloads.end())
        continue;
      ui.percentage->setText(std::format("{}%", std::lround(download->progress * 100)));
      ui.percentage->measure(renderer);
      ui.progress->setProgress(static_cast<float>(download->progress));
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
  // Playing media floods the capsule with its artwork, as Apple Music's player does; OSDs and
  // notifications shown meanwhile stay on it rather than dropping to the black capsule.
  if (gCupertino
      && cfg.mediaGradient
      && playing
      && !artPath.empty()
      && (view == island::View::Media
          || view == island::View::Activity
          || view == island::View::Osd
          || view == island::View::Notification)) {
    if (artPath != m_flowArt) {
      m_flowArt = artPath;
      auto art = loadImageFile(artPath, 32, true);
      if (!art || !m_flow.setArtwork(art->rgba, art->width, art->height))
        m_flow.clear();
    }
    showFlow(inst, m_flow.hasArtwork());
  } else {
    showFlow(inst, false);
  }
  const float s = inst.scale;
  auto [w, h] = island::size(
      view, cfg.height, cfg.clockSize, cfg.clockSeconds, cfg.calendarLabels != IslandCalendarLabels::Initials,
      cfg.mediaArtworkSize, !announcement.empty()
  );
  if (showSwitcher)
    w = std::max(w, 360.0F);
  w = island::batteryWidth(w, view, showBattery, showUnread);
  if (recording)
    w = std::max(w, 250.0F);
  if (compactView)
    w += 2 * privacyWidth;
  w = std::min(w, static_cast<float>(inst.surface->width()) / s - 16);
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
            if (inst.inside && !inst.badgeHovered && !inst.splitHovered()) {
              inst.hovered = true;
              refresh();
            }
          });
      });
      area->setOnClick([this, &inst, &split, index](const InputArea::PointerData&) {
        if (split.activity == island::Activity::None)
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
  // Critical notifications pulse with the capture glow (above) rather than taking an outline.
  inst.background->clearBorder();
  inst.root->setSize(static_cast<float>(inst.surface->width()), static_cast<float>(inst.surface->height()));
  const auto keyboardFocus = inst.input.captureTabFocus();
  const float activityOffset =
      inst.activityScroll && inst.previousView == view ? inst.activityScroll->scrollOffset() : 0;
  const bool viewChanged = inst.previousView != view;
  inst.previousView = view;
  inst.activityScroll = nullptr;
  inst.badgeHovered = false;
  inst.pressedAction.clear();
  const bool showMediaStatus = view == island::View::Activity && !showUnread && !showBattery && privacyList.empty();
  const bool showVisualizer = showMediaStatus && playing;
  std::unique_ptr<Node> retainedWidgets;
  if (expandedView && inst.hoverWidgets && inst.hoverWidgets->trayOnlyMode() == !showExtras)
    retainedWidgets = inst.hoverWidgets->parent()->removeChild(inst.hoverWidgets);
  inst.hoverWidgets = nullptr;
  std::unique_ptr<Node> retainedVisualizer;
  if (inst.visualizer && showVisualizer)
    retainedVisualizer = inst.content->removeChild(inst.visualizer);
  inst.visualizer = nullptr;
  if (inst.content) {
    auto previous = inst.background->removeChild(inst.content);
    // Switching views crossfades: the old content fades out over the new one fading in.
    if (viewChanged && previous && MotionService::instance().enabled() && !inst.skipCrossfade)
      crossfadeOut(inst, std::move(previous));
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
  inst.timerUi.clear();
  inst.downloadUi.clear();
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
  const auto sectionCard = [&](float top, float bottom) {
    if (bottom - top <= 6)
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

  if (view == island::View::DownloadActivity || view == island::View::TimerActivity) {
    const bool timerView = view == island::View::TimerActivity;
    const auto fraction = timerView ? std::optional{timers.front().fraction()}
        : capsuleDownloads.size() == 1 && capsuleDownloads.front().determinate
        ? std::optional{static_cast<float>(capsuleDownloads.front().progress)}
        : std::nullopt;
    DownloadRing* ringPtr = nullptr;
    if (!(outlineTimer || outlineDownload)) {
      auto ring =
          std::make_unique<DownloadRing>(36 * s, 2.5F * s, fraction, islandTint(kAppleBlue, ColorRole::Primary));
      ringPtr = ring.get();
      ring->setPosition(14 * s, (cfg.height - 36) * s / 2);
      canvas->addChild(std::move(ring));
    }
    const auto downloadIcon = capsuleDownloads.size() == 1 && !capsuleDownloads.front().icon.empty()
        ? capsuleDownloads.front().icon
        : "download";
    // A lone script activity names itself where the clock would be, like a Live Activity.
    const bool scriptTitle = !timerView
        && capsuleDownloads.size() == 1
        && !capsuleDownloads.front().icon.empty()
        && !capsuleDownloads.front().name.empty();
    glyph(
        timerView ? timers.front().icon : downloadIcon, 23, (cfg.height - 18) / 2, 18, islandRole(ColorRole::Primary)
    );
    const float inset = (showUnread ? 95.0F : 70.0F) + privacyWidth;
    const float available = std::max(1.0F, w - 2 * inset);
    const auto clockText = timerView ? countdownTime(timers.front())
        : scriptTitle                ? capsuleDownloads.front().name
                                     : time;
    const float textSize = scriptTitle ? std::min(cfg.clockSize, 16.0F) : cfg.clockSize;
    const auto metrics = renderer.measureText(
        clockText, textSize * s, FontWeight::Normal, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
    );
    const float clockSize =
        scriptTitle ? textSize : textSize * std::min(1.0F, available * s / std::max(1.0F, metrics.width));
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
    glyph(onlyDownloads ? "download" : "stack-2", 22, 18, 22, islandTint(kAppleBlue, ColorRole::Primary));
    label(
        i18n::tr(onlyDownloads ? "island.downloads.title" : "island.downloads.in-progress"), 56, 17, w - 78,
        Style::fontSizeTitle, foreground, false, 1, FontWeight::SemiBold
    );
    const auto rows = std::min(downloads.size(), std::size_t{4});
    for (std::size_t i = 0; i < rows; ++i) {
      const float y = 57 + static_cast<float>(i) * 55;
      sectionCard(y - 6, y + 49);
      const auto& download = downloads[i];
      // Cupertino leads each row with a round blue badge, as Apple lists transfers.
      const float textX = gCupertino ? 62 : 22;
      if (gCupertino)
        leadingBadge(download.icon.empty() ? "download" : download.icon, 22, y + 4, 30, kAppleBlue, ColorRole::Primary);
      label(download.name, textX, y, w - textX - 88, 13, foreground, false, 1, FontWeight::Normal, true);
      if (download.determinate) {
        auto* percentage =
            label(std::format("{}%", std::lround(download.progress * 100)), w - 76, y, 54, 13, muted, true);
        auto* bar = progress(static_cast<float>(download.progress), textX, y + 26, w - textX - 22, 7);
        inst.downloadUi.push_back({download.desktopId, percentage, bar});
      } else {
        label(i18n::tr("island.downloads." + download.phase), textX, y + 24, w - textX - 22, 12, muted);
      }
    }
    h = 60 + static_cast<float>(rows) * 55;
    if (downloads.size() > rows) {
      label(i18n::trp("island.downloads.more", downloads.size() - rows), 22, h, w - 44, 12, muted);
      h += 28;
    }
    if (player && cfg.hoverShowMedia && !showSwitcher) {
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
      clockLabel = label(
          announce ? announcement : time, inset, 0, w - inset * 2, announce ? 17 : size,
          recording ? islandRole(ColorRole::Error) : foreground, true
      );
      if (recording)
        inst.recordingLabel = clockLabel;
      const float clockY = (cfg.height * s - clockLabel->height()) / 2.0F + cfg.clockOffset * s;
      clockLabel->setPosition(
          inset * s, std::clamp(clockY, 0.0F, std::max(0.0F, cfg.height * s - clockLabel->height()))
      );
      action(0, 0, w, cfg.height, "controls", [panel, recording] {
        if (recording)
          ScreenRecorder::instance().stop();
        else
          panel("control-center");
      });
    }
    if (view == island::View::Activity) {
      artwork(12, (cfg.height - 38) / 2, 38);
      if (showMediaStatus && !playing)
        glyph("media-pause", w - 44, (cfg.height - 24) / 2, 24, muted);
      if (showVisualizer) {
        if (!retainedVisualizer)
          retainedVisualizer = std::make_unique<IslandAudioVisualizer>(m_spectrum, *inst.surface);
        inst.visualizer = static_cast<IslandAudioVisualizer*>(retainedVisualizer.get());
        // Foreground, not the accent: white on the black capsule, like Apple's Now Playing waveform.
        inst.visualizer->setGradient(foreground, foreground);
        inst.visualizer->setPosition((w - 44) * s, (cfg.height - 24) * s / 2);
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
  } else if (view == island::View::Media && player) {
    const float mediaOffset = std::max(0.0F, cfg.mediaArtworkSize - 56.0F);
    const float textX = 27.0F + cfg.mediaArtworkSize + 16.0F;
    artwork(27, 22 + (56.0F + mediaOffset - cfg.mediaArtworkSize) / 2.0F, cfg.mediaArtworkSize);
    label(
        player->title, textX, 27 + mediaOffset / 2.0F, w - textX - 25, Style::fontSizeTitle, foreground, false, 1,
        FontWeight::SemiBold, true
    );
    label(
        joinedArtists(player->artists), textX, 51 + mediaOffset / 2.0F, w - textX - 25, Style::fontSizeCaption, muted
    );
    const auto& source = player->identity.empty() ? player->desktopEntry : player->identity;
    if (!source.empty()) {
      label(source, textX, 73 + mediaOffset / 2.0F, w - textX - 42, Style::fontSizeMini, muted);
      glyph("chevron-right", w - 37, 73 + mediaOffset / 2.0F, 12, muted);
    }
    const float fraction =
        player->lengthUs > 0 ? static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs) : 0;
    // Twice the usual track height, centred where the thinner bar sat, inside the seek target.
    inst.seekProgress = progress(inst.seeking ? inst.seekFraction : fraction, 27, 104 + mediaOffset, w - 54, 8.0F);
    // Cupertino tints the track's progress with its artwork's most vivid colour.
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
      auto* mediaControl = control(x, 137 + mediaOffset, 44, 48, "", icon, tooltip, 23, available, std::move(cb));
      if (icon == "media-play" || icon == "media-pause") {
        mediaControl->inputArea()->setTabFocusKey("playback");
        // Apple's transport controls are bare symbols; the theme look keeps a filled play button.
        setIslandVariant(mediaControl, gCupertino ? ButtonVariant::Ghost : ButtonVariant::Default);
        mediaControl->setRadius(Style::scaledRadius(22, s));
      }
    };
    button(w / 2 - 89, "media-prev", i18n::tr("control-center.media.previous"), player->canGoPrevious, [this, bus] {
      m_mpris->previous(bus);
    });
    button(
        w / 2 - 22, playing ? "media-pause" : "media-play",
        i18n::tr(playing ? "control-center.media.pause" : "control-center.media.play"),
        playing ? player->canPause : player->canPlay, [this, bus] { m_mpris->playPause(bus); }
    );
    // Solid transport symbols, the same as the Control Center's media card.
    button(w / 2 + 45, "media-next", i18n::tr("control-center.media.next"), player->canGoNext, [this, bus] {
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
  } else if (view == island::View::Osd && m_osd && (m_osd->kind == OsdKind::Dnd || m_osd->kind == OsdKind::Charging)) {
    // Status pills, as the iPhone announces a Focus or a charger: a tinted symbol and title lead,
    // and the state (On, Off or the charge level) sits at the far end in the same tint.
    const bool charging = m_osd->kind == OsdKind::Charging;
    const bool on = charging || !m_osd->inactive;
    const Color tint = charging ? kAppleGreen : kAppleIndigo;
    const ColorRole role = charging ? ColorRole::Secondary : ColorRole::Primary;
    constexpr float badgeSize = 34.0F;
    const float badgeX = 16.0F;
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
      constexpr float symbolSize = 17.0F;
      glyph(
          icon, badgeX + (badgeSize - symbolSize) / 2, (h - symbolSize) / 2, symbolSize,
          on ? islandFixed(rgba(1.0F, 1.0F, 1.0F), 1.0F) : muted
      );
      const float textX = badgeX + badgeSize + 12;
      auto* name = label(
          i18n::tr(charging ? "island.status.charging" : "island.status.dnd"), textX, 0, w - textX - 20, 15, foreground,
          false, 1, FontWeight::SemiBold
      );
      auto* stateLine = label(
          charging ? std::format("{}%", std::lround(m_osd->progress * 100))
                   : i18n::tr(on ? "island.status.on" : "island.status.off"),
          textX, 0, w - textX - 20, 12, muted
      );
      const float block = name->height() + 2 * s + stateLine->height();
      name->setPosition(name->x(), (h * s - block) / 2);
      stateLine->setPosition(stateLine->x(), (h * s - block) / 2 + name->height() + 2 * s);
    } else {
      if (on)
        leadingBadge(icon, badgeX, (h - badgeSize) / 2, badgeSize, tint, role);
      else
        leadingBadge(icon, badgeX, (h - badgeSize) / 2, badgeSize, rgba(1.0F, 1.0F, 1.0F), ColorRole::OnSurfaceVariant);
      const auto state = charging ? std::format("{}%", std::lround(m_osd->progress * 100))
                                  : i18n::tr(on ? "island.status.on" : "island.status.off");
      const auto stateColor = on ? islandTint(tint, role) : muted;
      const auto stateMetrics = renderer.measureText(
          state, 15 * s, FontWeight::SemiBold, 0, 1, TextAlign::Start, m_config->config().shell.fontFamily
      );
      const float stateWidth = std::ceil(stateMetrics.width / s) + 2;
      auto* stateLabel =
          label(state, w - 22 - stateWidth, 0, stateWidth, 15, stateColor, false, 1, FontWeight::SemiBold);
      stateLabel->setPosition(stateLabel->x(), (h * s - stateLabel->height()) / 2);
      const float titleX = badgeX + badgeSize + 12;
      auto* title = label(
          i18n::tr(charging ? "island.status.charging" : "island.status.dnd"), titleX, 0,
          std::max(1.0F, w - 22 - stateWidth - 12 - titleX), 15, foreground, false, 1, FontWeight::SemiBold
      );
      title->setPosition(title->x(), (h * s - title->height()) / 2);
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
    // A screenshot's image is its thumbnail, not the sender's icon.
    const bool screenshot = n.category == kScreenshotNotificationCategory
        && n.imageData
        && n.imageData->width > 0
        && n.imageData->height > 0
        && n.imageData->channels == 4
        && n.imageData->data.size() >= static_cast<std::size_t>(n.imageData->rowStride) * n.imageData->height;
    Notification iconSource = n;
    if (screenshot)
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
    if ((expanded || inst.keyboardMode) && hasDefault)
      visibleActions.emplace_back("default", i18n::tr("notifications.actions.open"));
    for (std::size_t index = 0; index + 1 < n.actions.size() && visibleActions.size() < 3; index += 2)
      if (n.actions[index] != "default")
        visibleActions.emplace_back(n.actions[index], n.actions[index + 1]);
    // macOS keeps actions out of sight: hovering shows the one action, or "Options" for several,
    // in place of the time stamp, and only an opened notification (or keyboard mode) lists them.
    const bool actionsOpen = expanded || inst.keyboardMode;
    const bool hasActions = actionsOpen && !visibleActions.empty();
    const float maxHeight =
        std::min(expanded ? 640.0F : 360.0F, inst.outputHeight / s - 16.0F - (privacyList.empty() ? 0.0F : 32.0F));
    const float footerHeight = hasActions ? 46.0F : 16.0F;
    const float textBottom = maxHeight - footerHeight;
    const bool hasBody = n.body.find_first_not_of(" \t\r\n") != std::string::npos;
    // The thumbnail sits at the card's right, like an attachment on a macOS notification.
    constexpr float thumbnailHeight = 64.0F;
    const float thumbnailWidth = screenshot
        ? std::min(
              120.0F, thumbnailHeight * static_cast<float>(n.imageData->width) / static_cast<float>(n.imageData->height)
          )
        : 0.0F;
    const float textWidth =
        w - (expanded ? 64.0F : 44.0F) - (textX - 22.0F) - (screenshot ? thumbnailWidth + 12.0F : 0.0F);
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
    if (screenshot) {
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
      scrollView->setSize((w - 22 - textX) * s, viewportHeight * s);
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
  if (showSwitcher) {
    // Translate the main card and its manual hit regions together. Tabs remain
    // fixed above both the card and the scrolling footer.
    constexpr float tabHeight = 44;
    for (const auto& child : inst.content->children())
      child->setPosition(child->x(), child->y() + tabHeight * s);
    for (auto& hit : inst.actions)
      hit.y += tabHeight;
    if (inst.seek)
      inst.seekY += tabHeight;
    h += tabHeight;
    sectionCard(4, 44);
    const float tabWidth = (w - 32) / static_cast<float>(availableActivities.count());
    float x = 16;
    for (const auto activity : {island::Activity::Media, island::Activity::Downloads, island::Activity::Timers}) {
      if (!availableActivities.contains(activity))
        continue;
      const std::string key = activity == island::Activity::Media ? "media"
          : activity == island::Activity::Downloads               ? "downloads"
                                                                  : "timers";
      auto* tab = control(
          x, 8, tabWidth - 4, 30, i18n::tr("island.activities." + key), "", "", 0, true, [this, &inst, activity] {
            inst.activities.selected = activity;
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
      if (gCupertino) {
        const float x = w - 22 - controlsWidth + 10;
        const bool toggleAvailable = !timer.finished && timer.remaining > 0;
        auto* toggle = control(
            x, h + 8, 32, 32, "", timer.running ? "media-pause" : "media-play",
            i18n::tr(timer.running ? "island.timer.pause" : "island.timer.resume"), 16, toggleAvailable,
            [this, timer] { timerCommand(timer, timer.toggleCommand()); }
        );
        toggle->inputArea()->setTabFocusKey(timer.plugin + "-toggle");
        roundButton(toggle, kAppleOrange);
        auto* cancel =
            control(x + 40, h + 8, 32, 32, "", "x", i18n::tr("island.timer.cancel"), 16, true, [this, timer] {
              timerCommand(timer, timer.cancelCommand());
            });
        cancel->inputArea()->setTabFocusKey(timer.plugin + "-cancel");
        roundButton(cancel);
        auto* open = control(
            x + 80, h + 8, 32, 32, "", "chevron-right", i18n::tr("island.timer.open"), 16, true,
            [panel, timer] { panel(timer.panel); }
        );
        open->inputArea()->setTabFocusKey(timer.plugin + "-open");
        roundButton(open);
        h += 56;
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
  if (!privacyList.empty() || rowBell) {
    // Capture indicators are clickable icons in compact views and beside notifications,
    // and as a centred row in the expanded Island. Hovering names the capturing app.
    const float rowWidth = static_cast<float>(privacyList.size() + (rowBell ? 1 : 0)) * 24 + 8;
    const float x = compactView
        ? w - (showUnread ? (view == island::View::Activity ? 48 : 38) : 14) - (showBattery ? 42 : 0) - privacyWidth
        : (w - rowWidth + 8) / 2;
    const float y = compactView ? (cfg.height - 24) / 2 : h;
    for (std::size_t i = 0; i < privacyList.size(); ++i) {
      const auto& activity = privacyList[i];
      if (!compactView && !expandedView && activity.kind != PrivacyCaptureKind::Microphone) {
        glyph(activity.icon(), x + static_cast<float>(i) * 24 + 4, y + 4, 16, islandRole(ColorRole::Primary));
        continue;
      }
      auto* icon = control(
          x + static_cast<float>(i) * 24, y, 24, 24, "", slotBell ? "notification-unread" : activity.icon(),
          slotBell ? i18n::tr("notifications.unread-history")
                   : i18n::tr(activity.labelKey()) + ": " + activity.appNames(),
          16, true,
          [this, &inst, panel, slotBell, kind = activity.kind, binaries = activity.binaries] {
            if (slotBell) {
              panel("notifications");
              return;
            }
            if (kind == PrivacyCaptureKind::Microphone) {
              panel("audio");
              return;
            }
            // Camera and screen access are controlled by the capturing app: bring it forward,
            // or, when it has no window, expand the Island to name it.
            if (focusApp && focusApp(binaries))
              return;
            inst.suppressHover = false;
            inst.hovered = true;
            refresh();
          },
          10, 0
      );
      if (expandedView) {
        // In the expanded Island a button click that lends the surface to a panel loses the
        // panel's focus grab, so the icon keeps its hover and tooltip while the click falls
        // through to the Island's own action handling, as the media card's panel link does.
        icon->inputArea()->setAcceptedButtons(0);
        if (activity.kind == PrivacyCaptureKind::Microphone)
          action(x + static_cast<float>(i) * 24, y, 24, 24, "privacy-microphone", [panel] { panel("audio"); });
        else
          action(
              x + static_cast<float>(i) * 24, y, 24, 24, std::string("privacy-") + activity.icon(),
              [this, binaries = activity.binaries] {
                if (focusApp)
                  (void)focusApp(binaries);
              }
          );
      }
      auto iconPalette =
          (gCupertino ? islandButtonPalette(ButtonVariant::Ghost) : Button::defaultPalette(ButtonVariant::Ghost));
      iconPalette.normal.label = islandRole(ColorRole::Primary);
      icon->setCustomPalette(std::move(iconPalette));
      if (compactView) {
        // When the slot moves on, the outgoing icon rises and fades as the next rises into place.
        const std::string slotIcon = slotBell ? "notification-unread" : activity.icon();
        if (!inst.slotIcon.empty() && inst.slotIcon != slotIcon) {
          auto ghost = std::make_unique<Glyph>();
          ghost->setGlyph(inst.slotIcon);
          ghost->setGlyphSize(16 * s);
          ghost->setColor(islandRole(ColorRole::Primary));
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
              if (inst.inside && !inst.badgeHovered && !inst.splitHovered()) {
                inst.hovered = true;
                refresh();
              }
            });
        });
      }
    }
    if (rowBell) {
      const float bellX = x + static_cast<float>(privacyList.size()) * 24;
      auto* bell = control(
          bellX, y, 24, 24, "", "notification-unread", i18n::trp("notifications.unread-count", unreadCount), 16, true,
          [panel] { panel("notifications"); }, 10, 0
      );
      // As with the capture icons, the click goes through the Island's own action handling.
      bell->inputArea()->setAcceptedButtons(0);
      action(bellX, y, 24, 24, "unread-bell", [panel] { panel("notifications"); });
      auto bellPalette =
          (gCupertino ? islandButtonPalette(ButtonVariant::Ghost) : Button::defaultPalette(ButtonVariant::Ghost));
      bellPalette.normal.label = islandRole(ColorRole::Primary);
      bell->setCustomPalette(std::move(bellPalette));
    }
    if (!compactView)
      h += 32;
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
    batteryRing(batteryList.front(), w - 50 - (showUnread ? 36 : 0), (cfg.height - 36) / 2, 36, !outlineBattery);
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
          || view == island::View::TimerActivity)) {
    const float badgeX = view == island::View::Activity ? w - 44 : w - badgeWidth - 10;
    const float badgeY = (cfg.height - 24) / 2;
    auto* badge = control(
        badgeX, badgeY, badgeWidth, 24, "", "notification-unread", i18n::tr("notifications.unread-history"), 22, true,
        [panel] { panel("notifications"); }, 10, 0
    );
    badge->setRadius(Style::scaledRadius(12, s));
    auto badgePalette =
        (gCupertino ? islandButtonPalette(ButtonVariant::Ghost) : Button::defaultPalette(ButtonVariant::Ghost));
    badgePalette.normal.label = islandRole(ColorRole::Primary);
    badge->setCustomPalette(std::move(badgePalette));
    badge->setOnEnter([&inst] {
      inst.badgeHovered = true;
      inst.enter.stop();
    });
    badge->setOnLeave([this, &inst] {
      inst.badgeHovered = false;
      if (inst.inside && !inst.hovered && !inst.suppressHover)
        inst.enter.start(std::chrono::milliseconds(inst.config.hoverOpenDelayMs), [this, &inst] {
          if (inst.inside && !inst.badgeHovered && !inst.splitHovered()) {
            inst.hovered = true;
            refresh();
          }
        });
    });
  }
  if (view == island::View::Downloads) {
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
  if (expandedView
      && (showExtras || view == island::View::Media)
      && m_widgetFactory
      && (cfg.hoverShowTray
          || !cfg.hoverWidgets.empty()
          || !cfg.hoverWidgetsCenter.empty()
          || !cfg.hoverWidgetsRight.empty())) {
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
    inst.hoverWidgets->updateWidgets(renderer, std::max(1.0F, (w - 44) * s));
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
    const float available = std::max(1.0F, inst.outputHeight / s - footerTop - 24 - kExpandedBottomInset);
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
  inst.content->setSize(w * s, h * s);
  inst.content->layout(renderer);
  if (inst.activityScroll)
    inst.activityScroll->setScrollOffset(activityOffset);
  if (inst.keyboardMode) {
    inst.input.restoreTabFocus(keyboardFocus);
    if (!inst.input.focusedArea())
      (void)inst.input.cycleTabFocus(false);
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

void Island::crossfadeOut(Instance& inst, std::unique_ptr<Node> previous) {
  if (inst.outgoing != nullptr)
    (void)inst.background->removeChild(inst.outgoing);
  previous->setHitTestVisible(false);
  previous->setExcludeSubtreeFromTabOrder(true);
  // Behind the incoming content, which is added after it.
  inst.outgoing = inst.background->addChild(std::move(previous));
  Node* outgoing = inst.outgoing;
  inst.outgoingWidth = inst.targetWidth; // not yet the new view's
  const float startOpacity = outgoing->opacity();
  inst.outgoingFade = startOpacity > 0.01F ? 1.0F : 0.0F;
  inst.animations.animate(
      0, 1, kViewFadeOutMs, Easing::EaseOutCubic,
      [this, &inst, outgoing, startOpacity](float t) {
        outgoing->setOpacity(startOpacity * (1 - t));
        if (inst.outgoing == outgoing) {
          inst.outgoingFade = startOpacity > 0.01F ? 1 - t : 0.0F;
          geometry(inst);
        }
      },
      [this, &inst, outgoing] {
        if (inst.outgoing != outgoing)
          return;
        inst.outgoing = nullptr;
        inst.outgoingFade = 0.0F;
        geometry(inst);
        // Detach outside the animation tick; the instance or ghost may be gone by then.
        DeferredCall::callLater([this, instance = &inst, outgoing] {
          for (auto& ptr : m_instances) {
            if (ptr.get() != instance || ptr->background == nullptr)
              continue;
            const auto& children = ptr->background->children();
            if (std::ranges::any_of(children, [outgoing](const auto& child) { return child.get() == outgoing; }))
              (void)ptr->background->removeChild(outgoing);
          }
        });
      },
      outgoing
  );
  inst.contentFade = 0;
  inst.animations.animate(0, 1, kViewFadeInMs, Easing::EaseOutCubic, [this, &inst](float t) {
    inst.contentFade = t;
    geometry(inst);
  });
}

void Island::showFlow(Instance& inst, bool show) {
  if (inst.flowImage == nullptr)
    return;
  inst.flowShown = show;
  if (!show) {
    inst.flowImage->setVisible(false);
    if (std::ranges::none_of(m_instances, [](const auto& other) { return other->flowShown; }))
      m_flowTimer.stop();
    return;
  }
  // The caller has made this surface's context current.
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
      ))
    return;
  inst.flowImage->setExternalTexture(inst.surface->renderTarget().renderer(), inst.flowTexture);
  inst.flowImage->setVisible(true);
  inst.flowImage->markPaintDirty();
  if (!m_flowTimer.active() && MotionService::instance().enabled() && !visuals::ArtworkFlow::frozen())
    m_flowTimer.startRepeating(std::chrono::milliseconds(33), [this] { tickFlow(); });
}

void Island::tickFlow() {
  bool any = false;
  for (auto& ptr : m_instances) {
    auto& inst = *ptr;
    if (!inst.flowShown || inst.panelHosted || inst.flowTexture.id == 0 || !m_renderContext)
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
  if (!any && std::ranges::none_of(m_instances, [](const auto& other) { return other->flowShown; }))
    m_flowTimer.stop();
}

void Island::releaseFlow(Instance& inst) {
  if (inst.flowTexture.id != 0 && m_renderContext && inst.surface) {
    m_renderContext->makeCurrent(inst.surface->renderTarget());
    inst.surface->renderTarget().renderer().textureManager().unload(inst.flowTexture);
  }
  inst.flowTexture = {};
  inst.flowShown = false;
}

void Island::collapseAfterLeave(Instance& inst, std::chrono::milliseconds delay) {
  inst.leave.start(delay, [this, &inst] {
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
  inst.keyboardMode = false;
  inst.keyboardNotification.reset();
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

bool Island::focusKeyboard() {
  if (!enabled() || m_instances.empty() || ScreenRecorder::instance().active())
    return false;
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
      && (!m_mpris || !m_mpris->activePlayer())
      && progressActivities().empty()
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
          if (inst.inside && !inst.badgeHovered && !inst.splitHovered()) {
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
  if (inst.keyboardMode)
    releaseKeyboard(inst);
  inst.panelHosted = true;
  // Panels can be as tall as the output; the Island shrinks the surface again once it settles.
  inst.surfaceHeight = static_cast<std::uint32_t>(inst.outputHeight);
  inst.surface->requestSize(inst.surfaceWidth, inst.surfaceHeight);
  // The panel opens from the capsule alone; the split bubbles bud out again when it closes.
  for (auto& split : inst.splits) {
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
  inst.animations.cancelAll();
  // Finish any view crossfade the cancel cut short, so the Island comes back fully drawn.
  if (inst.outgoing != nullptr) {
    (void)inst.background->removeChild(inst.outgoing);
    inst.outgoing = nullptr;
  }
  inst.outgoingFade = 0.0F;
  inst.contentFade = 1.0F;
  inst.seeking = false;
  inst.activeSeek = {};
  inst.pressedAction.clear();
  inst.inside = false;
  inst.hovered = false;
  inst.heldMedia = false;
  inst.suppressHover = true;
  if (m_notification && m_notification->urgency != Urgency::Critical && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  return IslandPanelSurface{inst.surface.get(),       inst.output, inst.width * inst.scale,
                            inst.height * inst.scale, inst.scale,  inst.flowShown ? inst.flowTexture : TextureHandle{}};
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
      std::ranges::any_of(timers, [](const auto& timer) { return timer.active; })
  };
  const auto* instance = hosted != m_instances.end() ? hosted->get()
      : !m_instances.empty()                         ? m_instances.front().get()
                                                     : nullptr;
  const auto compact = island::preferredActivity(
      available, island::activityOrder(cfg.activityPriority),
      instance ? instance->compactActivity.selected() : island::Activity::None
  );
  const auto view = island::view(
      m_notification.has_value(), m_osd.has_value(), false, mediaActive, false, available.downloads, available.timers,
      true, true, island::Activity::None, compact
  );
  auto size = island::size(
      view, cfg.height, cfg.clockSize, cfg.clockSeconds, cfg.calendarLabels != IslandCalendarLabels::Initials,
      cfg.mediaArtworkSize, player && !player->title.empty() && trackPreview(cfg, output)
  );
  const auto batteryList = batteries(cfg, output);
  const bool unread =
      m_notifications && std::ranges::any_of(m_notifications->history(), [](const auto& item) { return !item.seen; });
  const auto privacyList = island::showsStatusIcons(view) ? privacy() : std::vector<island::PrivacyActivity>{};
  // The unread bell shares the privacy slot when both are active.
  size.width = island::batteryWidth(
      size.width, view, !batteryList.empty() && batteryList.front().compact(), unread && privacyList.empty()
  );
  if (!privacyList.empty()) {
    if (view == island::View::Rest
        || view == island::View::Activity
        || view == island::View::DownloadActivity
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
