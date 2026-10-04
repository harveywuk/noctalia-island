#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/core/thumbnail_service.h"
#include "shell/control_center/artwork_flow_layer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class AccountsService;
class AsyncTextureCache;
class Box;
class BrightnessService;
class PipeWireService;
class Slider;
class Button;
class CompositorPlatform;
class HttpClient;
class IpcService;
class ConfigService;
class DependencyService;
class Glyph;
class GridView;
class Image;
class InputArea;
class Label;
class Node;
class Shortcut;
class ScrollView;
class Wallpaper;
class ClipboardService;
namespace scripting {
  class ScriptApiContext;
}

// How a shortcut is drawn: a round toggle in the dashboard grid, or, in the Big Sur modules, a
// row of the connectivity module (toggle, name, state), the wide tile or a small tile.
enum class ShortcutPadKind : std::uint8_t { Grid, Row, Wide, Small };

struct ShortcutPad {
  // Survives HomeTab::onClose(); button/glyph/label are nulled with the scene.
  std::unique_ptr<Shortcut> shortcut;
  Button* button = nullptr;
  Glyph* glyph = nullptr;
  Label* label = nullptr;
  Label* status = nullptr;
  ShortcutPadKind kind = ShortcutPadKind::Grid;
};

class HomeTab : public Tab {
public:
  explicit HomeTab(const ControlCenterServices& services);
  ~HomeTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void layoutWallpaperBackground(Renderer& renderer);
  // The card fill is only the backdrop for an absent wallpaper: while one covers the card it is
  // cleared, so the card's rounded edge is the wallpaper's own and nothing shows around it.
  void syncUserCardFill();
  // Only the topmost ready wallpaper layer draws. Two stacked rounded images would each
  // antialias the same edge, hardening it; the placeholder steps aside once the crisp layer is
  // fully faded in.
  void syncWallpaperLayerVisibility();
  // Adds a card overlay for pointer and/or keyboard activation.
  struct CardOverlayOptions {
    bool keyboardFocus = true;
    bool pointerHitTest = true;
  };
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate);
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options);
  void layoutCardOverlays();
  void syncWallpaperBackground(Renderer& renderer);
  void ensureWallpaperThumbnail(const std::string& path, int targetPx);
  void startCrispFade();
  void cancelCrispFade();
  void sync(Renderer& renderer);
  void warnOnOversizedAvatarSource(const std::string& path);
  void syncScaledFonts();
  void syncShortcuts();
  void syncHeaderActions();
  bool resizeMediaArtToCard();
  void applyMediaOverlay(bool overlay);
  void onPanelCardOpacityChanged(float opacity) override;

  // The Big Sur modules layout (home_tab_modules.cpp).
  std::unique_ptr<Flex> createModules();
  void layoutModules(Renderer& renderer, float contentWidth, float bodyHeight);
  void syncModules();
  std::unique_ptr<Flex> makeShortcutModule(float scale);
  std::unique_ptr<Flex> makeSliderModule(
      float scale, const std::string& title, const std::string& detailTab, const std::string& detailTooltip,
      Slider** slider, Glyph** glyph, std::function<void(double)> onChange,
      std::function<void()> onDragEnd, std::unique_ptr<Node> trailing
  );
  std::unique_ptr<Flex> makeNowPlayingModule(float scale);
  void addPad(Flex& parent, std::unique_ptr<Shortcut> shortcut, ShortcutPadKind kind, float scale);
  // A pointer target over `target` (laid out in layoutModules) that runs `onActivate`.
  void addModuleOverlay(Flex& target, std::function<void()> onActivate);
  void flushBrightness();
  void flushVolume();
  // The configured shortcuts, reusing the instances kept across open/close.
  std::vector<std::unique_ptr<Shortcut>> takeShortcuts(std::size_t limit);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  Wallpaper* m_wallpaper = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  ShortcutServices m_services;
  bool m_active = false;

  bool m_stacked = false;
  ScrollView* m_homeScroll = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_bottomRow = nullptr;
  Flex* m_dateTimeCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Flex* m_mediaText = nullptr;
  Flex* m_userCard = nullptr;
  Flex* m_userMain = nullptr;
  InputArea* m_userAvatarArea = nullptr;
  Image* m_userAvatar = nullptr;
  Flex* m_userMonogram = nullptr;
  Label* m_userInitials = nullptr;

  Label* m_timeLabel = nullptr;
  Label* m_dateLabel = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherLine = nullptr;
  Label* m_userHost = nullptr;
  Label* m_userUptime = nullptr;
  Label* m_userVersion = nullptr;
  Button* m_settingsButton = nullptr;
  Button* m_sessionButton = nullptr;
  InputArea* m_userCardKeyboardArea = nullptr;
  InputArea* m_userCardArea = nullptr;
  InputArea* m_mediaCardArea = nullptr;
  InputArea* m_dateTimeCardArea = nullptr;
  // Survives onClose() so the oversized-source warning fires once per session.
  std::string m_sizeCheckedAvatarPath;

  // Two stacked layers: m_wallpaperPlaceholder shows the resident full-screen
  // wallpaper texture immediately (slightly soft), m_wallpaperBg holds the crisp
  // card-sized thumbnail and crossfades in over it once decoded.
  Image* m_wallpaperPlaceholder = nullptr;
  Image* m_wallpaperBg = nullptr;
  std::string m_loadedWallpaperPath;
  int m_loadedWallpaperSize = 0;
  std::string m_crispWorkingPath;
  int m_crispWorkingSize = 0;
  bool m_crispShown = false;
  bool m_crispNeedsFade = false;
  bool m_crispOpaque = false;
  bool m_placeholderReady = false;
  std::uint32_t m_wallpaperCrispAnimId = 0;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  Signal<>::ScopedConnection m_wallpaperChangedConn;

  Label* m_mediaTrack = nullptr;
  Label* m_mediaArtist = nullptr;
  Label* m_mediaStatus = nullptr;
  Label* m_mediaProgress = nullptr;
  Flex* m_mediaArtSlot = nullptr;
  Glyph* m_mediaArtFallback = nullptr;
  Image* m_mediaArt = nullptr;
  // The artwork flow behind the media tile, with its text turned white over it.
  Image* m_mediaBackdrop = nullptr;
  control_center::ArtworkFlowLayer m_mediaFlow;
  bool m_mediaPlaying = false;
  bool m_mediaOverlay = false;
  std::string m_loadedMediaArtUrl;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::string m_mediaPositionBusName;
  std::string m_mediaPositionTrackId;
  std::string m_mediaPositionTrackSignature;
  std::string m_mediaLastPlaybackStatus;
  std::int64_t m_mediaPositionUs = 0;
  std::chrono::steady_clock::time_point m_mediaPositionSampleAt;
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt;
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt;
  Timer m_progressTimer;
  // Keeps the home tab clock ticking while the panel is open and idle; the clock
  // otherwise only refreshes when an unrelated service forces a redraw.
  Timer m_clockTimer;

  // Big Sur modules.
  bool m_modules = false;
  BrightnessService* m_brightness = nullptr;
  ScrollView* m_modulesScroll = nullptr;
  Flex* m_modulesColumn = nullptr;
  Flex* m_shortcutModule = nullptr;
  Flex* m_displayModule = nullptr;
  Slider* m_displaySlider = nullptr;
  Glyph* m_displayGlyph = nullptr;
  Flex* m_soundModule = nullptr;
  Slider* m_soundSlider = nullptr;
  Glyph* m_soundGlyph = nullptr;
  Flex* m_mediaControls = nullptr;
  Button* m_mediaPlayButton = nullptr;
  Button* m_mediaNextButton = nullptr;
  std::vector<std::pair<Flex*, InputArea*>> m_moduleOverlays;
  float m_pendingBrightness = -1.0F;
  float m_pendingVolume = -1.0F;
  Timer m_brightnessTimer;
  Timer m_volumeTimer;
  // A slider moved by the user ignores service echoes until its writes have landed.
  std::chrono::steady_clock::time_point m_sliderHoldoff{};

  GridView* m_shortcutsGrid = nullptr;
  std::vector<ShortcutPad> m_shortcutPads;
  // Plugin config as of the last shortcut grid build. A plugin shortcut seeds its Luau
  // runtime at construction, so a change here must block instance reuse.
  PluginsConfig m_lastPlugins;
};
