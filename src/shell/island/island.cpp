#include "shell/island/island.h"
#include "shell/island/island_widget_host.h"
#include "shell/bar/widget_factory.h"
#include "capture/screen_recorder.h"
#include "pipewire/pipewire_spectrum.h"
#include "ui/visuals/audio_visualizer.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "core/log.h"
#include "dbus/downloads/download_progress_service.h"
#include "core/input/keybind_matcher.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_dispatcher.h"
#include "shell/island/island_state.h"
#include "shell/island/island_battery.h"
#include "shell/island/island_timer.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_state_store.h"
#include "render/animation/motion_service.h"
#include "time/time_format.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>

using namespace std::chrono_literals;

namespace {
  class IslandAudioVisualizer : public AudioVisualizer {
  public:
    IslandAudioVisualizer(PipeWireSpectrum* spectrum, LayerSurface& surface)
        : m_spectrum(spectrum), m_surface(surface) {
      setCentered(true);
      setMirrored(false);
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
  wl_output* output = nullptr;
  std::unique_ptr<LayerSurface> surface;
  AnimationManager animations;
  InputDispatcher input;
  std::unique_ptr<Node> root;
  Box* background = nullptr;
  Node* content = nullptr;
  ScrollView* activityScroll = nullptr;
  island::View previousView = island::View::Rest;
  float scale = 1;
  float width = 160;
  float height = 64;
  float targetWidth = 160;
  float targetHeight = 64;
  AnimationManager::Id morph = 0;
  bool panelHosted = false;
  bool inside = false;
  bool hovered = false;
  bool badgeHovered = false;
  bool keyboardMode = false;
  bool keyboardDownloads = false;
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
  // Reuse the shell's native ring and spinner renderers at the same size.
  class DownloadRing final : public Node {
  public:
    DownloadRing(float diameter, float thickness, std::optional<float> progress,
                 ColorRole role = ColorRole::Primary, bool charging = false) : m_role(role), m_charging(charging) {
      setSize(diameter, diameter);
      setHitTestVisible(false);
      if (!progress) {
        auto spinner = std::make_unique<Spinner>();
        spinner->setSpinnerSize(diameter);
        spinner->setThickness(thickness);
        spinner->setColor(colorSpecFromRole(ColorRole::Primary));
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
      if (!manager) { m_pulse = 0; return; }
      if (m_charging && !m_pulse) pulse();
    }
    void setProgress(float progress) {
      if (m_fill) { m_fill->setProgress(progress); m_fill->setVisible(progress > 0); }
    }

  private:
    void pulse() {
      if (!animationManager() || !MotionService::instance().enabled()) {
        if (m_fill) m_fill->setOpacity(1);
        return;
      }
      m_pulse = animationManager()->animate(0, 1, 1800, Easing::Linear,
          [this](float t) { m_fill->setOpacity(0.72F + 0.28F * std::cos(t * 6.2831853F)); },
          [this] { m_pulse = 0; pulse(); }, this);
    }
    void applyPalette() {
      m_track->setColor(resolveColorSpec(colorSpecFromRole(ColorRole::SurfaceVariant)));
      m_fill->setColor(resolveColorSpec(colorSpecFromRole(m_role)));
    }
    ColorRole m_role;
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
  const ColorSpec foreground = colorSpecFromRole(ColorRole::OnSurface);
  const ColorSpec muted = colorSpecFromRole(ColorRole::OnSurfaceVariant);
} // namespace

Island::Island() = default;
Island::~Island() { destroySurfaces(); }

void Island::initializeWidgets(const BarServices& services, IpcService* ipc) {
  m_widgetFactory = std::make_unique<WidgetFactory>(services);
  m_widgetActions.setIpcService(ipc);
}

void Island::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderer, MprisService* mpris,
    NotificationManager* notifications, HttpClient* http, SessionBus* bus, UPowerService* upower, BluetoothService* bluetooth,
    PipeWireService* pipewire, PipeWireSpectrum* spectrum
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

bool Island::enabled() const { return m_config && m_config->config().island.enabled; }
bool Island::osdVisible() const { return enabled() && m_osd.has_value(); }

std::vector<island::Battery> Island::batteries() const {
  return island::batterySnapshot(m_upower ? m_upower->batteryDevices() : std::vector<UPowerDeviceInfo>{},
      m_bluetooth ? m_bluetooth->devices() : std::vector<BluetoothDeviceInfo>{}, m_config->config().battery,
      m_upower ? m_upower->defaultSystemBattery() : nullptr);
}

std::vector<island::PrivacyActivity> Island::privacy() const {
  return m_privacySummary.snapshot(m_pipewire ? m_pipewire->privacyState() : PrivacyState{},
                                    m_config->config().shell.privacy);
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
    if (auto timer = island::timerSnapshot(value("noctalia/timer", "timer.state"),
        value("noctalia/timer", "timer.remaining"), value("noctalia/timer", "timer.duration"))) result.push_back(*timer);
  if (registry.hasEntry("thepunkoff/pomodoro:pomodoro"))
    if (auto timer = island::pomodoroSnapshot(value("thepunkoff/pomodoro", "pomodoro.state"),
        value("thepunkoff/pomodoro", "pomodoro.sessionData"))) result.push_back(*timer);
  std::ranges::stable_sort(result, [](const auto& a, const auto& b) {
    if (a.active != b.active) return a.active;
    if (a.running != b.running) return a.running;
    return a.plugin < b.plugin;
  });
  return result;
}

void Island::timerCommand(const island::Countdown& timer, const std::string& command) {
  if (!scripting::PluginRegistry::instance().hasEntry(timer.panel)) return;
  scripting::PluginStateStore::instance().set(timer.plugin, timer.commandKey(), nlohmann::json(command).dump());
  refresh();
}

void Island::destroySurfaces() {
  if (closeHostedPanel)
    closeHostedPanel();
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
    inst->surface->setSceneRoot(nullptr);
  }
  m_instances.clear();
}

void Island::onConfigReload() {
  if (m_notification && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  destroySurfaces();
  m_tick.stop();
  m_osd.reset();
  m_notification.reset();
  m_osdTimeout.stop();
  if (enabled()) {
    onOutputChange();
    m_tick.startRepeating(1s, [this] { refresh(); });
  }
}

void Island::onOutputChange() {
  if (!enabled() || !m_wayland || !m_renderContext)
    return;
  const auto& cfg = m_config->config().island;
  const auto selected = [&](const WaylandOutput& output) {
    return cfg.monitors.empty() || std::ranges::any_of(cfg.monitors, [&](const std::string& selector) {
             return outputMatchesSelector(selector, output);
           });
  };
  // Match Orbit's explicit monitor choice; never mirror private alerts to another output as a fallback.
  // End a borrow before resizing/removing an output's surface.
  if (closeHostedPanel && std::ranges::any_of(m_instances, [](const auto& inst) { return inst->panelHosted; }))
    closeHostedPanel();
  std::erase_if(m_instances, [&](const auto& inst) {
    const auto* output = m_wayland->findOutputByWl(inst->output);
    return !output || !output->done || !output->hasUsableGeometry() || !selected(*output);
  });
  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || !output.output || !output.hasUsableGeometry() || !selected(output))
      continue;
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
    const auto existing =
        std::ranges::find_if(m_instances, [&](const auto& inst) { return inst->output == output.output; });
    if (existing != m_instances.end()) {
      auto& current = **existing;
      current.scale = scale;
      current.signature.clear();
      current.surface->requestSize(sw, sh);
      current.surface->setExclusiveZone(cfg.reserveSpace ? static_cast<int>(std::ceil((cfg.height + 12) * scale)) : -1);
      current.surface->requestUpdate();
      continue;
    }
    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = scale;
    LayerSurfaceConfig surfaceConfig{
        .nameSpace = "noctalia-island",
        .layer = LayerShellLayer::Top,
        .anchor = LayerShellAnchor::Top,
        .width = sw,
        .height = sh,
        .exclusiveZone = cfg.reserveSpace ? static_cast<int>(std::ceil((cfg.height + 12) * inst->scale)) : -1,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = sw,
        .defaultHeight = sh,
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
        if (auto* button = dynamic_cast<Button*>(old->parent())) button->setKeyboardFocusHint(false);
      if (next)
        if (auto* button = dynamic_cast<Button*>(next->parent())) button->setKeyboardFocusHint(ptr->keyboardMode);
      if (next && ptr->keyboardMode && ptr->activityScroll) {
        auto* scroll = ptr->activityScroll;
        float top = 0;
        Node* node = next;
        for (; node && node != scroll->content(); node = node->parent()) top += node->y();
        if (node) {
          const float bottom = top + next->height();
          const float offset = scroll->scrollOffset();
          if (top < offset) scroll->setScrollOffset(top);
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
  if (m_notification && m_notification->timeout > 0 && std::ranges::none_of(m_instances, [](const auto& inst) {
        return inst->inside || inst->keyboardMode;
      }))
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  refresh();
}

void Island::refresh() {
  if (!enabled())
    return;
  hideDndSuppressed();
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const std::string track = player ? player->busName + logicalTrackSignature(*player) : "";
  if (track != m_trackSignature) {
    if (!m_trackSignature.empty() && player && player->playbackStatus == "Playing") {
      m_announcement = player->title;
      m_announcementTimeout.start(3500ms, [this] {
        m_announcement.clear();
        refresh();
      });
    }
    m_trackSignature = track;
  }
  if (!player || player->playbackStatus != "Playing")
    m_announcement.clear();
  for (auto& inst : m_instances)
    if (!inst->panelHosted)
      inst->surface->requestUpdate();
}

bool Island::showOsd(const OsdContent& content) {
  if (!enabled() || m_instances.empty())
    return false;
  // Track announcements belong in the clock slot, with no separate large OSD.
  if (content.kind == OsdKind::Media) {
    refresh();
    return true;
  }
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
    if (inst->keyboardMode && ((event == NotificationEvent::Closed && inst->keyboardNotification == n.id)
        || (event == NotificationEvent::Added && inst->keyboardNotification != n.id
            && !(m_notifications->doNotDisturb() && n.dndPolicy == NotificationDndPolicy::Respect))))
      releaseKeyboard(*inst);
  if (event == NotificationEvent::Closed) {
    if (m_notification && m_notification->id == n.id) {
      m_notification.reset();
      refresh();
    }
    return false;
  }
  if (!enabled() || m_instances.empty())
    return false;
  if (event == NotificationEvent::Updated && (!m_notification || m_notification->id != n.id))
    return true;
  // Preserve Noctalia's full reply editor for notifications requiring text input.
  for (std::size_t i = 0; i + 1 < n.actions.size(); i += 2)
    if (n.actions[i] == "inline-reply")
      return false;
  if (m_notifications->doNotDisturb() && n.dndPolicy == NotificationDndPolicy::Respect)
    return true;
  if (m_notification && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  if (!m_notification || m_notification->id != n.id)
    for (auto& inst : m_instances) inst->expandedNotification.reset();
  m_notification = n;
  if (n.timeout > 0 && std::ranges::any_of(m_instances, [](const auto& inst) { return inst->inside || inst->keyboardMode; }))
    m_notifications->pauseExpiry(n.id);
  refresh();
  return true;
}

void Island::hideDndSuppressed() {
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

void Island::dismissNotification() {
  if (!m_notification)
    return;
  const auto id = m_notification->id;
  m_notification.reset();
  m_notifications->close(id, CloseReason::Dismissed);
  refresh();
}

void Island::geometry(Instance& inst) {
  if (!inst.root)
    return;
  const float s = inst.scale;
  const float x = (static_cast<float>(inst.surface->width()) - inst.width * s) / 2;
  inst.background->setPosition(x, 8 * s);
  inst.background->setSize(inst.width * s, inst.height * s);
  inst.background->setRadius(std::min(inst.height / 2, 30.0F) * s);
  if (inst.content) {
    inst.content->setPosition((inst.width - inst.targetWidth) * s / 2, 0);
    // Conceal content until the expanding capsule has room to contain it.
    const float gap = std::max(std::abs(inst.width - inst.targetWidth), std::abs(inst.height - inst.targetHeight));
    inst.content->setOpacity(std::clamp(1 - gap / 55, 0.0F, 1.0F));
    inst.content->setHitTestVisible(inst.content->opacity() > 0.1F);
  }
  inst.surface->setInputRegion({InputRect{
      static_cast<int>(std::floor(x)), 0, static_cast<int>(std::ceil(inst.width * s)),
      static_cast<int>(std::ceil((inst.height + 8) * s))
  }});
  inst.surface->requestRedraw();
  inst.input.syncPointerHover();
  TooltipManager::instance().syncAnchor(inst.input.hoveredArea());
}

void Island::prepare(Instance& inst) {
  if (!enabled() || inst.panelHosted)
    return;
  UiPhaseScope phase(UiPhase::Layout);
  m_renderContext->makeCurrent(inst.surface->renderTarget());
  auto& renderer = inst.surface->renderTarget().renderer();
  const auto& cfg = m_config->config().island;
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const bool playing = player && player->playbackStatus == "Playing";
  const auto downloads = m_downloads ? m_downloads->active() : std::vector<DownloadProgress>{};
  const auto timers = countdowns();
  const bool timerActive = !timers.empty() && timers.front().active;
  const bool recording = ScreenRecorder::instance().active();
  if (inst.keyboardMode && (recording
      || (inst.keyboardNotification && (!m_notification || m_notification->id != inst.keyboardNotification))
      || (!inst.keyboardNotification && inst.keyboardDownloads != !downloads.empty())
      || (!inst.keyboardNotification && !player && downloads.empty() && !timerActive)))
    releaseKeyboard(inst);
  const bool expansionRequested = inst.hovered || inst.keyboardMode;
  if (player && expansionRequested && (playing || inst.keyboardMode))
    inst.heldMedia = true;
  if (!player || !expansionRequested)
    inst.heldMedia = false;
  const auto view = recording ? island::View::Rest : island::view(m_notification.has_value(), m_osd.has_value() && !inst.keyboardMode, expansionRequested, playing, inst.heldMedia, !downloads.empty(), timerActive, cfg.hoverShowMedia, cfg.hoverShowDownloads);
  const bool compactView = view == island::View::Rest || view == island::View::Activity || view == island::View::DownloadActivity || view == island::View::TimerActivity;
  const bool expandedView = view == island::View::Calendar || view == island::View::Media || view == island::View::Downloads;
  const auto batteryList = !recording && (compactView || expandedView) ? batteries() : std::vector<island::Battery>{};
  const bool showBattery = compactView && !batteryList.empty() && batteryList.front().compact();
  const auto privacyList = privacy();
  const float privacyWidth = privacyList.empty() ? 0 : static_cast<float>(privacyList.size()) * 24 + 8;
  const auto unreadCount = m_notifications
      ? std::ranges::count_if(m_notifications->history(), [](const auto& entry) { return !entry.seen; }) : 0;
  const bool showUnread = unreadCount > 0 && !recording
      && (view == island::View::Rest || view == island::View::Activity
          || view == island::View::Calendar || view == island::View::Media
          || view == island::View::DownloadActivity || view == island::View::Downloads || view == island::View::TimerActivity);
  constexpr float badgeWidth = 24.0F;
  // A gesture belongs to the track and card where it started.
  if (inst.seeking && (view != island::View::Media || !player || !player->canSeek
                      || inst.seekTrackSignature != m_trackSignature || inst.seekLengthUs != player->lengthUs)) {
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
        + actionSignature + (inst.expandedNotification == m_notification->id ? "expanded" : "collapsed");
    break;
  case island::View::Osd:
    signature += std::format("{}|{}|{}|{}|{}", static_cast<int>(m_osd->kind), m_osd->value, m_osd->icon, m_osd->progress, m_osd->showProgress);
    break;
  case island::View::Media:
    signature += std::format(
        "{}|{}|{}|{}|{}", m_trackSignature, playing, artPath, player ? player->title : "",
        player ? joinedArtists(player->artists) : ""
    );
    if (player)
      signature += std::format("|{}|{}|{}|{}|{}|{}|{}|{}", player->lengthUs, player->canPlay, player->canPause,
                               player->canGoPrevious, player->canGoNext, player->canSeek, player->identity, player->desktopEntry);
    break;
  case island::View::Activity:
    signature += time + m_announcement + artPath;
    break;
  case island::View::Calendar:
    signature += time + date;
    break;
  case island::View::Rest:
    signature += recording ? "recording" : time;
    break;
  case island::View::TimerActivity:
    break;
  case island::View::DownloadActivity:
  case island::View::Downloads:
    signature += view == island::View::Downloads ? std::to_string(player.has_value()) : time;
    for (const auto& download : downloads)
      signature += std::format("|{}|{}|{}|{}|{}", download.desktopId, download.name,
                               view == island::View::Downloads ? 0L : std::lround(download.progress * 100), download.determinate, download.phase);
    break;
  }
  if (view != island::View::Notification && view != island::View::Osd)
    signature += std::format("|unread:{}|count:{}", showUnread,
        showUnread && (view == island::View::Calendar || view == island::View::Media || view == island::View::Downloads) ? unreadCount : 0);
  signature += std::format("|keyboard:{}", inst.keyboardMode);
  if (expandedView || view == island::View::TimerActivity)
    for (const auto& timer : timers)
      signature += std::format("|timer:{}|{}|{}|{}|{}|{}|{}", timer.plugin, timer.titleKey, timer.running,
          timer.active, timer.finished, timer.duration, timer.session);
  for (const auto& activity : privacyList)
    signature += std::format("|privacy:{}:{}", static_cast<int>(activity.kind), activity.appNames());
  if (showBattery || expandedView)
    for (const auto& battery : batteryList)
      signature += std::format("|battery:{}|{}|{}|{}|{}|{}|{}", battery.id, battery.name, battery.icon,
          std::lround(battery.percentage), static_cast<int>(battery.state), battery.seconds / 60, battery.low);
  if (expandedView && inst.hoverWidgets) {
    const float oldHeight = inst.hoverWidgets->height();
    inst.hoverWidgets->updateWidgets(renderer, inst.hoverWidgets->width());
    if (oldHeight != inst.hoverWidgets->height()) inst.signature.clear();
  }
  if (signature == inst.signature && inst.root) {
    // Timer ticks must not rebuild the stop action between pointer press and release.
    if (recording && inst.recordingLabel) {
      inst.recordingLabel->setText(time);
      inst.recordingLabel->measure(renderer);
      const float clockY = (cfg.height * inst.scale - inst.recordingLabel->height()) / 2.0F
          + cfg.clockOffset * inst.scale;
      inst.recordingLabel->setPosition(inst.recordingLabel->x(), std::clamp(clockY, 0.0F,
          std::max(0.0F, cfg.height * inst.scale - inst.recordingLabel->height())));
    }
    for (const auto& ui : inst.timerUi) {
      const auto timer = std::ranges::find(timers, ui.plugin, &island::Countdown::plugin);
      if (timer == timers.end()) continue;
      ui.label->setText(timer->time());
      ui.label->measure(renderer);
      ui.setFraction(timer->fraction());
    }
    for (const auto& ui : inst.downloadUi) {
      const auto download = std::ranges::find(downloads, ui.desktopId, &DownloadProgress::desktopId);
      if (download == downloads.end()) continue;
      ui.percentage->setText(std::format("{}%", std::lround(download->progress * 100)));
      ui.percentage->measure(renderer);
      ui.progress->setProgress(static_cast<float>(download->progress));
    }
    // Keep the title's marquee alive while the playback position advances.
    if (view == island::View::Media && player) {
      if (inst.seekProgress)
        inst.seekProgress->setProgress(inst.seeking ? inst.seekFraction : player->lengthUs > 0
            ? std::clamp(static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs), 0.0F, 1.0F)
            : 0.0F);
      if (inst.mediaPosition) {
        inst.mediaPosition->setText(std::format("{}:{:02}", displayPosition / 60, displayPosition % 60));
        inst.mediaPosition->setColor(inst.seeking ? colorSpecFromRole(ColorRole::Primary) : muted);
        inst.mediaPosition->measure(renderer);
      }
    }
    return;
  }
  inst.signature = signature;
  const float s = inst.scale;
  auto [w, h] = island::size(view, cfg.height, cfg.clockSize, cfg.clockSeconds,
                            cfg.calendarLabels != IslandCalendarLabels::Initials, cfg.mediaArtworkSize);
  w = island::batteryWidth(w, view, showBattery, showUnread);
  if (recording) w = std::max(w, 250.0F);
  if (compactView) w += 2 * privacyWidth;
  w = std::min(w, static_cast<float>(inst.surface->width()) / s - 16);
  if (!inst.root) {
    inst.root = std::make_unique<Node>();
    inst.root->setAnimationManager(&inst.animations);
    auto box = std::make_unique<Box>();
    box->setFill(colorSpecFromRole(ColorRole::Surface));
    box->setClipChildren(true);
    inst.background = box.get();
    inst.root->addChild(std::move(box));
    inst.surface->setSceneRoot(inst.root.get());
    inst.input.setSceneRoot(inst.root.get());
    inst.width = w;
    inst.height = h;
  }
  inst.root->setSize(static_cast<float>(inst.surface->width()), static_cast<float>(inst.surface->height()));
  const auto keyboardFocus = inst.input.captureTabFocus();
  const float activityOffset = inst.activityScroll && inst.previousView == view ? inst.activityScroll->scrollOffset() : 0;
  inst.previousView = view;
  inst.activityScroll = nullptr;
  inst.badgeHovered = false;
  inst.pressedAction.clear();
  const bool showVisualizer = view == island::View::Activity && !showUnread && !showBattery && privacyList.empty();
  std::unique_ptr<Node> retainedWidgets;
  if (expandedView && inst.hoverWidgets)
    retainedWidgets = inst.hoverWidgets->parent()->removeChild(inst.hoverWidgets);
  inst.hoverWidgets = nullptr;
  std::unique_ptr<Node> retainedVisualizer;
  if (inst.visualizer && showVisualizer)
    retainedVisualizer = inst.content->removeChild(inst.visualizer);
  inst.visualizer = nullptr;
  if (inst.content)
    inst.background->removeChild(inst.content);
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

  const auto label = [&](std::string text, float x, float y, float width, float size, ColorSpec color = foreground,
                         bool center = false, int lines = 1, FontWeight weight = FontWeight::Normal,
                         bool scroll = false) {
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
  const auto glyph = [&](const std::string& name, float x, float y, float size, ColorSpec color = foreground) {
    auto node = std::make_unique<Glyph>();
    node->setGlyph(name);
    node->setGlyphSize(size * s);
    node->setColor(color);
    node->measure(renderer);
    node->setPosition(x * s, y * s);
    canvas->addChild(std::move(node));
  };
  const auto progress = [&](float value, float x, float y, float width, float height = 5.0F) {
    auto node = std::make_unique<ProgressBar>();
    node->setTrack(colorSpecFromRole(ColorRole::SurfaceVariant));
    node->setFill(foreground);
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
                           const std::string& icon, const std::string& tooltip, float iconSize,
                           bool available, std::function<void()> cb, float fontSize = 12, float padding = -1) {
    auto node = std::make_unique<Button>();
    node->setVariant(ButtonVariant::Ghost);
    if (!text.empty()) node->setText(text);
    if (!icon.empty()) node->setGlyph(icon);
    if (!text.empty()) node->setFontSize(fontSize * s);
    if (!icon.empty()) node->setGlyphSize(iconSize * s);
    node->setPadding(padding >= 0 ? padding * s : (text.empty() ? 0 : 8 * s));
    node->setRadius(14 * s);
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
  const auto panel = [this, &inst](const std::string& name) {
    if (inst.keyboardMode) releaseKeyboard(inst);
    inst.suppressHover = true;
    inst.hovered = false;
    inst.heldMedia = false;
    if (openPanel)
      openPanel(inst.output, name);
    refresh();
  };
  const auto artwork = [&](float x, float y, float size) {
    auto image = std::make_unique<Image>();
    image->setSize(size * s, size * s);
    image->setRadius(10 * s);
    image->setFit(ImageFit::Cover);
    image->setPosition(x * s, y * s);
    // Crop before downsampling so wide thumbnails retain a full-resolution
    // square. Keep extra detail for fractional scaling; Image applies buffer scale.
    const int decodeSize = static_cast<int>(std::ceil(size * s * 2.0F));
    if (!artPath.empty()
        && image->setSourceFile(renderer, artPath, decodeSize, true, true))
      inst.content->addChild(std::move(image));
    else
      glyph("disc", x + 4, y + 4, size - 8);
  };

  if (view == island::View::DownloadActivity || view == island::View::TimerActivity) {
    const bool timerView = view == island::View::TimerActivity;
    const auto fraction = timerView ? std::optional{timers.front().fraction()} : downloads.size() == 1 && downloads.front().determinate
        ? std::optional{static_cast<float>(downloads.front().progress)} : std::nullopt;
    auto ring = std::make_unique<DownloadRing>(36 * s, 2.5F * s, fraction);
    auto* ringPtr = ring.get();
    ring->setPosition(14 * s, (cfg.height - 36) * s / 2);
    canvas->addChild(std::move(ring));
    glyph(timerView ? timers.front().icon : "download", 23, (cfg.height - 18) / 2, 18, colorSpecFromRole(ColorRole::Primary));
    const float inset = (showUnread ? 95.0F : 70.0F) + privacyWidth;
    const float available = std::max(1.0F, w - 2 * inset);
    const auto clockText = timerView ? timers.front().time() : time;
    const auto metrics = renderer.measureText(clockText, cfg.clockSize * s, FontWeight::Normal, 0, 1, TextAlign::Start,
                                              m_config->config().shell.fontFamily);
    const float clockSize = cfg.clockSize * std::min(1.0F, available * s / std::max(1.0F, metrics.width));
    auto* clockLabel = label(clockText, inset, 0, available, clockSize, foreground, true);
    if (timerView) inst.timerUi.push_back({timers.front().plugin, clockLabel, [ringPtr](float value) { ringPtr->setProgress(value); }});
    clockLabel->setPosition(inset * s, std::clamp((cfg.height * s - clockLabel->height()) / 2 + cfg.clockOffset * s,
        0.0F, std::max(0.0F, cfg.height * s - clockLabel->height())));
    if (timerView && !showBattery && privacyList.empty()) {
      glyph(timers.front().running ? "player-pause" : timers.front().finished ? "check" : "player-play",
            w - (showUnread ? 80 : 46), (cfg.height - 18) / 2, 18, muted);
    } else if (!timerView && !showBattery && privacyList.empty() && (downloads.size() > 1 || downloads.front().determinate)) {
      const auto value = downloads.size() == 1
          ? std::format("{}%", std::lround(downloads.front().progress * 100))
          : std::to_string(downloads.size());
      auto* status = label(value, w - (showUnread ? 91 : 70), 0, 50, 12, muted, true);
      status->setPosition(status->x(), (cfg.height * s - status->height()) / 2);
    }
    action(0, 0, w, cfg.height, "downloads", [this, &inst] {
      inst.suppressHover = false;
      inst.hovered = true;
      refresh();
    });
  } else if (view == island::View::Downloads) {
    glyph("download", 22, 18, 22, colorSpecFromRole(ColorRole::Primary));
    label(i18n::tr("island.downloads.title"), 56, 17, w - 78, 16);
    const auto rows = std::min(downloads.size(), std::size_t{4});
    for (std::size_t i = 0; i < rows; ++i) {
      const float y = 57 + static_cast<float>(i) * 55;
      const auto& download = downloads[i];
      label(download.name, 22, y, w - 110, 13, foreground, false, 1, FontWeight::Normal, true);
      if (download.determinate) {
        auto* percentage = label(std::format("{}%", std::lround(download.progress * 100)), w - 76, y, 54, 13, muted, true);
        auto* bar = progress(static_cast<float>(download.progress), 22, y + 26, w - 44, 7);
        inst.downloadUi.push_back({download.desktopId, percentage, bar});
      } else {
        label(i18n::tr("island.downloads." + download.phase), 22, y + 24, w - 44, 12, muted);
      }
    }
    h = 60 + static_cast<float>(rows) * 55;
    if (downloads.size() > rows) {
      label(i18n::trp("island.downloads.more", downloads.size() - rows), 22, h, w - 44, 12, muted);
      h += 28;
    }
    if (player && cfg.hoverShowMedia) {
      control(22, h, w - 44, 32, i18n::tr("island.downloads.media"), "", "", 0, true,
              [panel] { panel("media"); });
      h += 38;
    }

  } else if (view == island::View::Rest || view == island::View::Activity || view == island::View::Calendar) {
    float size = view == island::View::Calendar ? cfg.clockSize * 1.4F : cfg.clockSize;
    const bool announce = view == island::View::Activity && !m_announcement.empty();
    const float inset = (view == island::View::Activity ? (showBattery && showUnread ? 96.0F : 65.0F)
        : view == island::View::Rest && showBattery ? (showUnread ? 88.0F : 56.0F)
        : (view == island::View::Rest && showUnread ? badgeWidth + 12.0F : 12.0F)) + (compactView ? privacyWidth : 0);
    if (view == island::View::Rest && (showUnread || showBattery)) {
      // Symmetric space keeps the clock centred, including large-font settings.
      const auto metrics = renderer.measureText(time, size * s, FontWeight::Normal, 0, 1, TextAlign::Start,
                                                m_config->config().shell.fontFamily);
      const float available = (w - inset * 2) * s;
      if (metrics.width > available)
        size *= available / metrics.width;
    }
    Label* clockLabel = nullptr;
    if (view != island::View::Calendar || cfg.hoverShowClock) {
      clockLabel = label(
          announce ? m_announcement : time, inset, 0, w - inset * 2, announce ? 17 : size,
          recording ? colorSpecFromRole(ColorRole::Error) : foreground, true
      );
      if (recording)
        inst.recordingLabel = clockLabel;
      const float clockY = (cfg.height * s - clockLabel->height()) / 2.0F + cfg.clockOffset * s;
      clockLabel->setPosition(inset * s, std::clamp(clockY, 0.0F, std::max(0.0F, cfg.height * s - clockLabel->height())));
      action(0, 0, w, cfg.height, "controls", [panel, recording] {
        if (recording) ScreenRecorder::instance().stop(); else panel("control-center");
      });
    }
    if (view == island::View::Activity) {
      artwork(12, (cfg.height - 38) / 2, 38);
      if (showVisualizer) {
        if (!retainedVisualizer)
          retainedVisualizer = std::make_unique<IslandAudioVisualizer>(m_spectrum, *inst.surface);
        inst.visualizer = static_cast<IslandAudioVisualizer*>(retainedVisualizer.get());
        inst.visualizer->setPosition((w - 44) * s, (cfg.height - 24) * s / 2);
        inst.visualizer->setSize(24 * s, 24 * s);
        canvas->addChild(std::move(retainedVisualizer));
      }
    }
    if (view == island::View::Calendar) {
      h = (cfg.hoverShowClock ? cfg.height : 0) + (cfg.hoverShowCalendar ? 72 : 8);
      // Orbit's clock sits in a taller head band above a strip centred on today.
      if (clockLabel && cfg.hoverShowCalendar) {
        const float expandedY = ((cfg.height + 19.0F) * s - clockLabel->height()) / 2.0F + cfg.expandedClockOffset * s;
        clockLabel->setPosition(inset * s, std::clamp(expandedY, 0.0F, std::max(0.0F, (cfg.height + 6.0F) * s - clockLabel->height())));
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
          const bool today = day == 3;
          const int distance = std::abs(day - 3);
          const float grade = today ? 1.0F : 1.0F - static_cast<float>(distance - 1) * 0.07F;
          auto color = colorSpecFromRole(today ? ColorRole::Primary : ColorRole::OnSurfaceVariant);
          color.alpha *= std::max(0.2F, 1.0F - static_cast<float>(distance) * 0.27F);
          char dayName[64]{};
          std::strftime(dayName, sizeof(dayName), "%a", &tm);
          const bool abbreviated = cfg.calendarLabels == IslandCalendarLabels::Abbreviated
              || (today && cfg.calendarLabels == IslandCalendarLabels::TodayAbbreviated);
          char* shortName = g_utf8_substring(dayName, 0, abbreviated ? 3 : 1);
          const float x = stripX + static_cast<float>(day) * cellWidth;
          auto* weekday = label(
              shortName, x, stripY, cellWidth, daySize,
              color, true, 1, today ? FontWeight::Bold : FontWeight::SemiBold
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
    label(player->title, textX, 27 + mediaOffset / 2.0F, w - textX - 25, 15,
          foreground, false, 1, FontWeight::Normal, true);
    label(joinedArtists(player->artists), textX, 51 + mediaOffset / 2.0F, w - textX - 25, 12, muted);
    const auto& source = player->identity.empty() ? player->desktopEntry : player->identity;
    if (!source.empty()) {
      label(source, textX, 73 + mediaOffset / 2.0F, w - textX - 42, 10, muted);
      glyph("chevron-right", w - 37, 73 + mediaOffset / 2.0F, 12, muted);
    }
    const float fraction =
        player->lengthUs > 0 ? static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs) : 0;
    inst.seekProgress = progress(inst.seeking ? inst.seekFraction : fraction, 27, 106 + mediaOffset, w - 54);
    inst.mediaPosition = label(std::format("{}:{:02}", displayPosition / 60, displayPosition % 60),
                              27, 118 + mediaOffset, 65, 10,
                              inst.seeking ? colorSpecFromRole(ColorRole::Primary) : muted);
    const auto seconds = player->lengthUs / 1000000;
    label(std::format("{}:{:02}", seconds / 60, seconds % 60), w - 73, 118 + mediaOffset, 46, 10, muted, true);
    const std::string bus = player->busName;
    const auto button = [&](float x, const std::string& icon, const std::string& tooltip,
                            bool available, std::function<void()> cb) {
      auto* mediaControl = control(x, 137 + mediaOffset, 44, 48, "", icon, tooltip, 23, available, std::move(cb));
      if (icon == "player-play" || icon == "player-pause") mediaControl->inputArea()->setTabFocusKey("playback");
    };
    button(w / 2 - 89, "player-skip-back", i18n::tr("control-center.media.previous"),
           player->canGoPrevious, [this, bus] { m_mpris->previous(bus); });
    button(
        w / 2 - 22, playing ? "player-pause" : "player-play",
        i18n::tr(playing ? "control-center.media.pause" : "control-center.media.play"),
        playing ? player->canPause : player->canPlay,
        [this, bus] { m_mpris->playPause(bus); }
    );
    button(w / 2 + 45, "player-skip-forward", i18n::tr("control-center.media.next"),
           player->canGoNext, [this, bus] { m_mpris->next(bus); });
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
  } else if (view == island::View::Osd && m_osd && m_osd->kind == OsdKind::LockKeys) {
    constexpr float iconSize = 26.0F;
    constexpr float gap = 12.0F;
    const float maxTextWidth = std::max(1.0F, w - 40 - iconSize - gap);
    const auto metrics = renderer.measureText(m_osd->value, 15 * s, FontWeight::Normal, 0, 1, TextAlign::Start,
                                              m_config->config().shell.fontFamily);
    const float textWidth = std::min(maxTextWidth, metrics.width / s);
    const float x = (w - iconSize - gap - textWidth) / 2;
    glyph(m_osd->icon, x, (h - iconSize) / 2, iconSize);
    auto* value = label(m_osd->value, x + iconSize + gap, 0, textWidth, 15);
    value->setPosition(value->x(), (h * s - value->height()) / 2);
  } else if (view == island::View::Osd && m_osd) {
    glyph(m_osd->icon, 20, 19, 26);
    if (m_osd->kind == OsdKind::Volume && m_osd->showProgress) {
      const float barHeight = cfg.volumeBarHeight;
      if (cfg.volumeShowPercentage)
        label(m_osd->value, 60, 12, w - 80, 15);
      const float barCenter = cfg.volumeShowPercentage ? 44.0F : h / 2.0F;
      progress(m_osd->progress, 60, barCenter - barHeight / 2.0F, w - 85, barHeight);
    } else {
      label(m_osd->value, 60, m_osd->showProgress ? 12 : 22, w - 80, 15);
      if (m_osd->showProgress)
        progress(m_osd->progress, 60, 43, w - 85);
    }
  } else if (view == island::View::Notification && m_notification) {
    const auto n = *m_notification;
    const bool expanded = inst.expandedNotification == n.id;
    auto* appLabel = label(n.appName, 22, 14, w - 75, 11, muted);
    control(w - 47, 5, 32, 30, "", "x", i18n::tr("notifications.dismiss"), 18, true,
            [this] { dismissNotification(); });
    std::vector<std::pair<std::string, std::string>> visibleActions;
    const bool hasDefault = std::ranges::find(n.actions, "default") != n.actions.end();
    if ((expanded || inst.keyboardMode) && hasDefault)
      visibleActions.emplace_back("default", i18n::tr("notifications.actions.open"));
    for (std::size_t index = 0; index + 1 < n.actions.size() && visibleActions.size() < 3; index += 2)
      if (n.actions[index] != "default")
        visibleActions.emplace_back(n.actions[index], n.actions[index + 1]);
    const bool hasActions = !visibleActions.empty();
    const float maxHeight = std::min(expanded ? 640.0F : 360.0F,
                                     static_cast<float>(inst.surface->height()) / s - 16.0F
                                         - (privacyList.empty() ? 0.0F : 32.0F));
    const float footerHeight = hasActions ? 60.0F : 16.0F;
    const float textBottom = maxHeight - footerHeight;
    const bool hasBody = n.body.find_first_not_of(" \t\r\n") != std::string::npos;
    const float textWidth = w - (expanded ? 64.0F : 44.0F);
    auto* summary = label(n.summary, 22, 37, textWidth, 16, foreground, false, 0);
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
      body = label(n.body, 22, bodyY, textWidth, 13, muted, false, 0);
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
      if (body->visible()) contentBottom = bodyY + body->height() / s;
    }
    if (expanded) {
      auto scroll = std::make_unique<ScrollView>();
      scroll->setContentScale(s);
      scroll->setViewportPaddingH(0);
      scroll->setViewportPaddingV(0);
      scroll->content()->setGap(8 * s);
      scroll->content()->addChild(inst.content->removeChild(summary));
      if (body) scroll->content()->addChild(inst.content->removeChild(body));
      const float viewportHeight = std::min(contentBottom - 37.0F, std::max(1.0F, textBottom - 37.0F));
      auto* scrollView = scroll.get();
      inst.content->addChild(std::move(scroll));
      scrollView->setSize((w - 44) * s, viewportHeight * s);
      scrollView->layout(renderer);
      scrollView->setPosition(22 * s, 37 * s);
      contentBottom = 37.0F + viewportHeight;
    }
    const auto toggleExpanded = [this, &inst, id = n.id] {
      if (inst.expandedNotification == id) inst.expandedNotification.reset();
      else inst.expandedNotification = id;
      refresh();
    };
    if (expanded || truncated) {
      appLabel->setMinWidth((w - 112) * s);
      appLabel->setMaxWidth((w - 112) * s);
      appLabel->measure(renderer);
      auto* expandControl = control(w - 82, 5, 32, 30, "", expanded ? "chevron-up" : "chevron-down",
              i18n::tr(expanded ? "notifications.collapse" : "notifications.expand"),
              18, true, toggleExpanded);
      expandControl->inputArea()->setTabFocusKey("notification-expand");
    }
    h = contentBottom + footerHeight;
    const float actionsY = contentBottom + 8.0F;
    const float actionWidth = hasActions ? (w - 44) / static_cast<float>(visibleActions.size()) : 0;
    for (std::size_t index = 0; index < visibleActions.size(); ++index) {
      const auto& [key, text] = visibleActions[index];
      auto* actionControl = control(22 + static_cast<float>(index) * actionWidth, actionsY, actionWidth - 8, 44,
              text, "", text, 18, true, [this, id = n.id, key] {
        (void)m_notifications->invokeAction(id, key);
      });
      actionControl->inputArea()->setTabFocusKey("notification-action-" + key);
    }
    if (!expanded)
      action(0, 37, w, contentBottom - 37.0F, "notification", [this, n, panel, truncated, toggleExpanded] {
        if (truncated) toggleExpanded();
        else if (std::ranges::find(n.actions, "default") != n.actions.end())
          (void)m_notifications->invokeAction(n.id, "default");
        else {
          dismissNotification();
          panel("notifications");
        }
      });
  }
  // Keep the main activity fixed while a busy footer scrolls within the output.
  const float footerTop = h;
  auto footer = std::make_unique<Node>();
  if (expandedView) canvas = footer.get();
  if (expandedView && cfg.hoverShowTimers && !timers.empty()) {
    auto displayTimers = timers;
    // Compact priority can change on pause; keep hover buttons under the same pointer.
    std::ranges::sort(displayTimers, {}, &island::Countdown::plugin);
    for (const auto& timer : displayTimers) {
      if (!timer.active) continue;
      auto ring = std::make_unique<DownloadRing>(36 * s, 2.5F * s, timer.fraction());
      auto* ringPtr = ring.get();
      ring->setPosition(22 * s, (h + 6) * s);
      canvas->addChild(std::move(ring));
      glyph(timer.icon, 31, h + 15, 18, colorSpecFromRole(ColorRole::Primary));
      label(i18n::tr(timer.titleKey), 70, h + 3, w - 165, 13);
      auto* remaining = label(timer.time(), w - 94, h + 3, 72, 16, foreground, true);
      inst.timerUi.push_back({timer.plugin, remaining, [ringPtr](float value) { ringPtr->setProgress(value); }});
      label(i18n::tr(timer.finished ? "island.timer.finished" : timer.running ? "island.timer.running" : "island.timer.paused"),
            70, h + 25, w - 92, 11, muted);
      h += 48;
      const float buttonWidth = (w - 60) / 3;
      auto* toggle = control(22, h, buttonWidth, 30,
          i18n::tr(timer.running ? "island.timer.pause" : "island.timer.resume"), "", "", 0,
          !timer.finished && timer.remaining > 0, [this, timer] { timerCommand(timer, timer.toggleCommand()); });
      toggle->inputArea()->setTabFocusKey(timer.plugin + "-toggle");
      auto* cancel = control(30 + buttonWidth, h, buttonWidth, 30, i18n::tr("island.timer.cancel"), "", "", 0, true,
                             [this, timer] { timerCommand(timer, timer.cancelCommand()); });
      cancel->inputArea()->setTabFocusKey(timer.plugin + "-cancel");
      auto* open = control(38 + 2 * buttonWidth, h, buttonWidth, 30, i18n::tr("island.timer.open"), "", "", 0, true,
                           [panel, timer] { panel(timer.panel); });
      open->inputArea()->setTabFocusKey(timer.plugin + "-open");
      h += 38;
    }
    // A discoverable entry point when idle, without adding a permanent compact badge.
    if (view == island::View::Calendar) {
      const auto idle = std::ranges::count_if(timers, [](const auto& timer) { return !timer.active; });
      if (idle > 0) {
        const float count = static_cast<float>(idle);
        const float width = (w - 44 - (count - 1) * 8) / count;
        float x = 22;
        for (const auto& timer : timers) {
          if (timer.active) continue;
          control(x, h, width, 32, i18n::tr(timer.pomodoro ? "island.timer.pomodoro" : "island.timer.title"),
                  timer.icon, "", 16, true, [panel, timer] { panel(timer.panel); });
          x += width + 8;
        }
        h += 40;
      }
    }
  }
  if (expandedView) {
    for (const auto& activity : privacyList) {
      if (activity.kind == PrivacyCaptureKind::Microphone) {
        auto* icon = control(18, h + 5, 36, 36, "", activity.icon(), i18n::tr("island.privacy.audio-controls"), 20, true,
                             [panel] { panel("audio"); });
        auto iconPalette = Button::defaultPalette(ButtonVariant::Ghost);
        iconPalette.normal.label = colorSpecFromRole(ColorRole::Primary);
        icon->setCustomPalette(std::move(iconPalette));
      } else
        glyph(activity.icon(), 26, h + 13, 20, colorSpecFromRole(ColorRole::Primary));
      label(i18n::tr(activity.labelKey()), 66, h + 4, w - 88, 13);
      label(activity.appNames(), 66, h + 26, w - 88, 12, muted, false, 1, FontWeight::Normal, true);
      h += 54;
    }
  } else if (!privacyList.empty()) {
    // Keep capture indicators visible even while a notification or OSD occupies the island.
    const float x = compactView ? w - (showUnread ? (view == island::View::Activity ? 48 : 38) : 14)
        - (showBattery ? 42 : 0) - privacyWidth : (w - privacyWidth + 8) / 2;
    const float y = compactView ? (cfg.height - 24) / 2 : h;
    for (std::size_t i = 0; i < privacyList.size(); ++i) {
      const auto& activity = privacyList[i];
      if (!compactView && activity.kind != PrivacyCaptureKind::Microphone) {
        glyph(activity.icon(), x + static_cast<float>(i) * 24 + 4, y + 4, 16, colorSpecFromRole(ColorRole::Primary));
        continue;
      }
      auto* icon = control(x + static_cast<float>(i) * 24, y, 24, 24, "", activity.icon(),
          i18n::tr(activity.labelKey()) + ": " + activity.appNames(), 16, true,
          [this, &inst, panel, kind = activity.kind] {
            if (kind == PrivacyCaptureKind::Microphone) { panel("audio"); return; }
            // Camera and screen access are controlled by the capturing app; expose its name here.
            inst.suppressHover = false; inst.hovered = true; refresh();
          }, 10, 0);
      auto iconPalette = Button::defaultPalette(ButtonVariant::Ghost);
      iconPalette.normal.label = colorSpecFromRole(ColorRole::Primary);
      icon->setCustomPalette(std::move(iconPalette));
      if (compactView) {
        icon->setOnEnter([&inst] { inst.badgeHovered = true; inst.enter.stop(); });
        icon->setOnLeave([this, &inst] {
          inst.badgeHovered = false;
          if (inst.inside && !inst.hovered && !inst.suppressHover)
            inst.enter.start(110ms, [this, &inst] {
              if (inst.inside && !inst.badgeHovered) { inst.hovered = true; refresh(); }
            });
        });
      }
    }
    if (!compactView) h += 32;
  }
  const auto batteryRing = [&](const island::Battery& battery, float x, float y, float diameter) {
    const auto role = battery.low ? ColorRole::Error : ColorRole::Primary;
    auto ring = std::make_unique<DownloadRing>(diameter * s, 2.5F * s,
        static_cast<float>(battery.percentage / 100.0), role, battery.charging());
    ring->setPosition(x * s, y * s);
    canvas->addChild(std::move(ring));
    glyph(battery.charging() ? "battery-charging" : battery.icon, x + (diameter - 18) / 2,
          y + (diameter - 18) / 2, 18, colorSpecFromRole(role));
  };
  if (showBattery)
    batteryRing(batteryList.front(), w - 50 - (showUnread ? 36 : 0), (cfg.height - 36) / 2, 36);
  if (expandedView && cfg.hoverShowBatteries && !batteryList.empty()) {
    const auto rows = std::min(batteryList.size(), std::size_t{4});
    for (std::size_t i = 0; i < rows; ++i) {
      const auto& battery = batteryList[i];
      batteryRing(battery, 22, h + 7, 36);
      label(battery.name, 70, h + 4, w - 150, 13, foreground, false, 1, FontWeight::Normal, true);
      label(std::format("{}%", std::lround(battery.percentage)), w - 76, h + 4, 54, 13,
            colorSpecFromRole(battery.low ? ColorRole::Error : ColorRole::OnSurface), true);
      auto detail = batteryStateLabel(battery.state);
      if (battery.seconds > 0)
        detail += " · " + formatDuration(std::chrono::seconds(battery.seconds)) + " "
            + i18n::tr(battery.charging() ? "island.battery.until-full" : "island.battery.remaining");
      label(detail, 70, h + 26, w - 92, 11, muted);
      h += 54;
    }
    if (batteryList.size() > rows) {
      label(i18n::trp("island.battery.more", batteryList.size() - rows), 22, h, w - 44, 12, muted);
      h += 26;
    }
    h += 6;
  }
  if (showUnread && expandedView && cfg.hoverShowUnread) {
    control(22, h, w - 44, 24, i18n::trp("notifications.unread-count", unreadCount), "",
            i18n::tr("notifications.unread-history"), 0, true, [panel] { panel("notifications"); });
    h += 32;
  }
  if (showUnread && (view == island::View::Rest || view == island::View::Activity || view == island::View::DownloadActivity || view == island::View::TimerActivity)) {
    const float badgeX = view == island::View::Activity ? w - 44 : w - badgeWidth - 10;
    const float badgeY = (cfg.height - 24) / 2;
    auto* badge = control(badgeX, badgeY, badgeWidth, 24, "",
                          "bell", i18n::tr("notifications.unread-history"), 22, true,
                          [panel] { panel("notifications"); }, 10, 0);
    badge->setRadius(12 * s);
    auto badgePalette = Button::defaultPalette(ButtonVariant::Ghost);
    badgePalette.normal.label = colorSpecFromRole(ColorRole::Primary);
    badge->setCustomPalette(std::move(badgePalette));
    badge->setOnEnter([&inst] { inst.badgeHovered = true; inst.enter.stop(); });
    badge->setOnLeave([this, &inst] {
      inst.badgeHovered = false;
      if (inst.inside && !inst.hovered && !inst.suppressHover)
        inst.enter.start(110ms, [this, &inst] {
          if (inst.inside && !inst.badgeHovered) { inst.hovered = true; refresh(); }
        });
    });
  }
  if (view == island::View::Downloads) {
    control(22, h, w - 44, 32, i18n::tr("island.downloads.close"), "", "", 0, true, [this, &inst] {
      if (inst.keyboardMode) releaseKeyboard(inst);
      else { inst.hovered = false; inst.suppressHover = inst.inside; inst.enter.stop(); refresh(); }
    });
    h += 42;
  }
  if (expandedView && m_widgetFactory
      && (!cfg.hoverWidgets.empty() || !cfg.hoverWidgetsCenter.empty() || !cfg.hoverWidgetsRight.empty())) {
    if (!retainedWidgets) {
      retainedWidgets = std::make_unique<IslandWidgetHost>(
          *m_widgetFactory, m_config->config(), inst.output, s, &inst.animations, &m_widgetActions,
          [&inst] { if (!inst.panelHosted) inst.surface->requestUpdate(); },
          [&inst] { if (!inst.panelHosted) inst.surface->requestRedraw(); },
          [&inst] { if (!inst.panelHosted) inst.surface->requestFrameTick(); });
    }
    inst.hoverWidgets = static_cast<IslandWidgetHost*>(retainedWidgets.get());
    inst.hoverWidgets->updateWidgets(renderer, std::max(1.0F, (w - 44) * s));
    inst.hoverWidgets->setPosition(22 * s, (h + 8) * s);
    const float widgetHeight = inst.hoverWidgets->height() / s;
    footer->addChild(std::move(retainedWidgets));
    if (widgetHeight > 0) h += widgetHeight + 16;
  }
  if (expandedView) {
    const float footerHeight = h - footerTop;
    for (const auto& child : footer->children()) child->setPosition(child->x(), child->y() - footerTop * s);
    footer->setSize(w * s, footerHeight * s);
    const float available = std::max(1.0F, static_cast<float>(inst.surface->height()) / s - footerTop - 24);
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
  if (expandedView) h = std::max(h, cfg.height);
  inst.content->setSize(w * s, h * s);
  inst.content->layout(renderer);
  if (inst.activityScroll) inst.activityScroll->setScrollOffset(activityOffset);
  if (inst.keyboardMode) {
    inst.input.restoreTabFocus(keyboardFocus);
    if (!inst.input.focusedArea()) (void)inst.input.cycleTabFocus(false);
  }
  if (w != inst.targetWidth || h != inst.targetHeight) {
    inst.animations.cancel(inst.morph);
    const float oldW = inst.width, oldH = inst.height;
    inst.targetWidth = w;
    inst.targetHeight = h;
    inst.morph = inst.animations.animate(0, 1, 420, Easing::EaseOutCubic, [this, &inst, oldW, oldH, w, h](float value) {
      inst.width = oldW + (w - oldW) * value;
      inst.height = oldH + (h - oldH) * value;
      geometry(inst);
    });
  }
  geometry(inst);
}

void Island::releaseKeyboard(Instance& inst) {
  inst.keyboardMode = false;
  inst.keyboardDownloads = false;
  inst.keyboardNotification.reset();
  inst.hovered = false;
  inst.heldMedia = false;
  inst.suppressHover = inst.inside;
  inst.enter.stop();
  inst.leave.stop();
  inst.input.setFocus(nullptr);
  inst.surface->setKeyboardInteractivity(LayerShellKeyboard::None);
  if (m_notification && m_notification->timeout > 0
      && std::ranges::none_of(m_instances, [](const auto& other) { return other->inside || other->keyboardMode; }))
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  inst.signature.clear();
  refresh();
}

bool Island::focusKeyboard() {
  if (!enabled() || m_instances.empty() || ScreenRecorder::instance().active()) return false;
  if (closeHostedPanel) closeHostedPanel();
  for (auto& inst : m_instances)
    if (inst->keyboardMode) releaseKeyboard(*inst);
  auto found = std::ranges::find_if(m_instances, [this](const auto& inst) {
    return inst->output == m_wayland->lastPointerOutput();
  });
  auto& inst = **(found == m_instances.end() ? m_instances.begin() : found);
  const auto timers = countdowns();
  if (!m_notification && (!m_mpris || !m_mpris->activePlayer()) && (!m_downloads || m_downloads->active().empty())
      && std::ranges::none_of(timers, [](const auto& timer) { return timer.active; })) {
    if (!openPanel) return false;
    openPanel(inst.output, "calendar");
    return true;
  }
  m_osd.reset();
  m_osdTimeout.stop();
  inst.keyboardMode = true;
  inst.keyboardDownloads = m_downloads && !m_downloads->active().empty();
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
  if (!m_wayland) return false;
  for (auto& item : m_instances) {
    auto& inst = *item;
    if (!inst.keyboardMode || inst.panelHosted || m_wayland->lastKeyboardSurface() != inst.surface->wlSurface())
      continue;
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      const auto notification = inst.keyboardNotification;
      releaseKeyboard(inst);
      if (notification && m_notification && m_notification->id == notification) dismissNotification();
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
      inst.input.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      inst.leave.stop();
      if (m_notification && m_notification->timeout > 0)
        m_notifications->pauseExpiry(m_notification->id);
      if (!inst.suppressHover)
        inst.enter.start(110ms, [this, &inst] {
          if (inst.inside && !inst.badgeHovered) {
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
      if (m_notification && m_notification->timeout > 0 && std::ranges::none_of(m_instances, [](const auto& other) {
            return other->inside || other->keyboardMode;
          }))
        m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
      inst.leave.start(180ms, [this, &inst] {
        inst.hovered = false;
        inst.heldMedia = false;
        refresh();
      });
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
      inst.input.pointerAxis(static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis,
                             event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
                             event.axisLines, event.axisGestureSerial);
    } else if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && inst.content) {
      // A newly rebuilt card may still be concealed during the size transition.
      if (inst.content->opacity() <= 0.1F) {
        inst.pressedAction.clear();
        inst.input.cancelPointerCapture();
        return true;
      }
      if (!inst.seeking && inst.input.pointerButton(static_cast<float>(event.sx), static_cast<float>(event.sy),
                                                   event.button, event.pressed, event.serial, event.time, event.touch)) {
        // A click can lend this surface to a panel. Drop any hover restored by
        // the button dispatcher after its callback, including pending tooltips.
        if (inst.panelHosted) {
          inst.input.pointerLeave();
          TooltipManager::instance().forceDestroy();
        }
        return true;
      }
      const float x = (static_cast<float>(event.sx) - inst.background->x() - inst.content->x()) / inst.scale;
      const float y = (static_cast<float>(event.sy) - 8 * inst.scale) / inst.scale;
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

std::optional<IslandPanelSurface> Island::acquirePanelSurface(wl_output* output, bool exactOutput) {
  if (!enabled() || m_instances.empty())
    return std::nullopt;
  auto it = std::ranges::find_if(m_instances, [output](const auto& inst) { return inst->output == output; });
  if (exactOutput && it == m_instances.end()) return std::nullopt;
  // Shell shortcuts on another monitor still open the configured island.
  auto& inst = **(it == m_instances.end() ? m_instances.begin() : it);
  if (inst.panelHosted)
    return std::nullopt;
  if (inst.keyboardMode) releaseKeyboard(inst);
  inst.panelHosted = true;
  if (inst.visualizer) {
    inst.content->removeChild(inst.visualizer);
    inst.visualizer = nullptr;
  }
  inst.input.pointerLeave();
  TooltipManager::instance().forceDestroy();
  inst.enter.stop();
  inst.leave.stop();
  inst.animations.cancelAll();
  inst.seeking = false;
  inst.activeSeek = {};
  inst.pressedAction.clear();
  inst.inside = false;
  inst.hovered = false;
  inst.heldMedia = false;
  inst.suppressHover = true;
  if (m_notification && m_notification->timeout > 0)
    m_notifications->resumeExpiry(m_notification->id, m_notification->timeout);
  return IslandPanelSurface{
      inst.surface.get(), inst.output, inst.width * inst.scale, inst.height * inst.scale, inst.scale
  };
}

island::Size Island::panelReturnSize() const {
  const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
  const bool playing = player && player->playbackStatus == "Playing";
  const auto timers = countdowns();
  const auto view = island::view(m_notification.has_value(), m_osd.has_value(), false, playing, false,
                               m_downloads && !m_downloads->active().empty(),
                               std::ranges::any_of(timers, [](const auto& timer) { return timer.active; }));
  const auto& cfg = m_config->config().island;
  auto size = island::size(view, cfg.height, cfg.clockSize, cfg.clockSeconds,
                          cfg.calendarLabels != IslandCalendarLabels::Initials, cfg.mediaArtworkSize);
  const auto batteryList = batteries();
  const bool unread = m_notifications && std::ranges::any_of(m_notifications->history(), [](const auto& item) { return !item.seen; });
  size.width = island::batteryWidth(size.width, view, !batteryList.empty() && batteryList.front().compact(), unread);
  const auto privacyList = privacy();
  if (!privacyList.empty()) {
    if (view == island::View::Rest || view == island::View::Activity || view == island::View::DownloadActivity || view == island::View::TimerActivity)
      size.width += 2 * (static_cast<float>(privacyList.size()) * 24 + 8);
    else if (view == island::View::Osd || view == island::View::Notification)
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
    inst.suppressHover = false;
    inst.width = inst.targetWidth = width / inst.scale;
    inst.height = inst.targetHeight = height / inst.scale;
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
    inst.surface->setLayer(LayerShellLayer::Top);
    inst.surface->setBlurRegion({});
    inst.surface->requestUpdate();
  }
}
