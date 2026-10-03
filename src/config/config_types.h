#pragma once

#include "config/color_spec.h"
#include "config/config_limits.h"
#include "config/widget_setting_value.h"
#include "core/font_defaults.h"
#include "core/input/key_chord.h"
#include "system/sysmon_threshold_profile.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

struct WaylandOutput;

// Direction hidden accordion members unfold relative to the always-visible first member, along the
// bar lane's main axis.
enum class BarAccordionDirection : std::uint8_t { End = 0, Start = 1 };

// A capsule group: an ordered set of member widgets sharing one capsule + style. `id` is opaque and
// auto-generated. A group appears in a bar lane as a single token (see makeCapsuleGroupToken); its
// members live inside the group, not loose in the lane.
struct BarCapsuleGroupStyle {
  std::string id;
  std::vector<std::string> members; // ordered member widget references
  bool enabled = true;
  ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // True when `border` is explicitly present (empty value = no outline); mirrors bar/widget border semantics.
  bool borderSpecified = false;
  std::optional<ColorSpec> border;
  std::optional<ColorSpec> foreground;
  float padding = Style::barCapsulePadding;
  std::optional<float> radius;
  float opacity = 1.0F;
  // Collapse the group to its first member; hovering the capsule reveals the rest inline.
  bool accordion = false;
  BarAccordionDirection accordionDirection = BarAccordionDirection::End;
  // Gap between members inside the capsule, in logical pixels; unset inherits the bar's widget_spacing.
  std::optional<std::int32_t> widgetSpacing;

  bool operator==(const BarCapsuleGroupStyle&) const = default;
};

// A lane entry referencing a capsule group is the literal "group:" prefix + the group id. The colon
// cannot appear in a widget instance id, so group tokens never collide with widget references.
inline constexpr std::string_view kCapsuleGroupTokenPrefix = "group:";
[[nodiscard]] bool isCapsuleGroupToken(std::string_view laneEntry);
[[nodiscard]] std::string capsuleGroupTokenId(std::string_view laneEntry);
[[nodiscard]] std::string makeCapsuleGroupToken(std::string_view groupId);

enum class IslandCalendarLabels : std::uint8_t { Initials, TodayAbbreviated, Abbreviated };
// Cupertino: always black with white content and per-activity tints, like Apple's
// Dynamic Island. Theme: follows the shell palette.
enum class IslandAppearance : std::uint8_t { Cupertino, Theme };

enum class IslandActivityPriority : std::uint8_t {
  TimersDownloadsMedia,
  TimersMediaDownloads,
  DownloadsTimersMedia,
  DownloadsMediaTimers,
  MediaTimersDownloads,
  MediaDownloadsTimers
};
struct IslandConfig {
  // Keep the original key as the left group for existing configurations.
  std::vector<std::string> hoverWidgets;
  std::vector<std::string> hoverWidgetsCenter;
  std::vector<std::string> hoverWidgetsRight;
  bool hoverShowClock = true;
  bool hoverShowCalendar = true;
  bool hoverShowMedia = true;
  bool hoverShowDownloads = true;
  bool hoverShowTimers = true;
  bool hoverShowBatteries = true;
  bool hoverShowUnread = true;
  bool hoverShowTray = true;
  // A flowing gradient made from the artwork fills the Island while media plays (Cupertino look).
  bool mediaGradient = true;
  bool enabled = false;
  std::vector<std::string> monitors;
  float height = 64.0F;
  float clockSize = 24.0F;
  float scale = 1.0F;
  bool reserveSpace = true;
  bool clockSeconds = false;
  float clockOffset = 0.0F;
  float expandedClockOffset = 0.0F;
  IslandCalendarLabels calendarLabels = IslandCalendarLabels::Abbreviated;
  IslandAppearance appearance = IslandAppearance::Cupertino;
  bool outerProgressRing = false;
  float mediaArtworkSize = 56.0F;
  IslandActivityPriority activityPriority = IslandActivityPriority::TimersDownloadsMedia;
  // A second running activity detaches into a round bubble beside the capsule, as on iPhone.
  bool splitActivities = true;
  bool cycleActivities = false;
  int activityCycleSeconds = 5;
  int hoverOpenDelayMs = 110;
  int hoverCloseDelayMs = 180;
  int trackPreviewSeconds = 5;
  int pausedMediaSeconds = 3;
  int bluetoothPreviewSeconds = 5;
  bool revealOnTrackChange = true;
  // Minutes before a calendar event starts that its countdown appears; 0 turns it off.
  int upNextMinutes = 10;
  std::string trackPreviewMonitor = "all";
  std::string bluetoothPreviewMonitor = "all";
  float volumeBarHeight = 18.0F;
  bool volumeShowPercentage = false;
  bool operator==(const IslandConfig&) const = default;
};
enum class BarPresentation : std::uint8_t { Standard, Island };
struct IslandMonitorOverride {
  std::optional<std::vector<std::string>> hoverWidgets;
  std::optional<std::vector<std::string>> hoverWidgetsCenter;
  std::optional<std::vector<std::string>> hoverWidgetsRight;
  std::optional<bool> hoverShowClock;
  std::optional<bool> hoverShowCalendar;
  std::optional<bool> hoverShowMedia;
  std::optional<bool> hoverShowDownloads;
  std::optional<bool> hoverShowTimers;
  std::optional<bool> hoverShowBatteries;
  std::optional<bool> hoverShowUnread;
  std::optional<bool> hoverShowTray;
  std::optional<bool> mediaGradient;
  std::optional<float> height;
  std::optional<float> clockSize;
  std::optional<bool> clockSeconds;
  std::optional<float> clockOffset;
  std::optional<float> expandedClockOffset;
  std::optional<IslandCalendarLabels> calendarLabels;
  std::optional<bool> outerProgressRing;
  std::optional<float> mediaArtworkSize;
  std::optional<IslandActivityPriority> activityPriority;
  std::optional<bool> splitActivities;
  std::optional<bool> cycleActivities;
  std::optional<int> activityCycleSeconds;
  std::optional<int> hoverOpenDelayMs;
  std::optional<int> hoverCloseDelayMs;
  std::optional<int> trackPreviewSeconds;
  std::optional<int> pausedMediaSeconds;
  std::optional<int> bluetoothPreviewSeconds;
  std::optional<bool> revealOnTrackChange;
  std::optional<std::string> trackPreviewMonitor;
  std::optional<std::string> bluetoothPreviewMonitor;
  std::optional<float> volumeBarHeight;
  std::optional<bool> volumeShowPercentage;
  bool operator==(const IslandMonitorOverride&) const = default;
};
inline IslandConfig applyIslandOverride(IslandConfig base, const IslandMonitorOverride& override) {
  if (override.hoverWidgets)
    base.hoverWidgets = *override.hoverWidgets;
  if (override.hoverWidgetsCenter)
    base.hoverWidgetsCenter = *override.hoverWidgetsCenter;
  if (override.hoverWidgetsRight)
    base.hoverWidgetsRight = *override.hoverWidgetsRight;
  if (override.hoverShowClock)
    base.hoverShowClock = *override.hoverShowClock;
  if (override.hoverShowCalendar)
    base.hoverShowCalendar = *override.hoverShowCalendar;
  if (override.hoverShowMedia)
    base.hoverShowMedia = *override.hoverShowMedia;
  if (override.hoverShowDownloads)
    base.hoverShowDownloads = *override.hoverShowDownloads;
  if (override.hoverShowTimers)
    base.hoverShowTimers = *override.hoverShowTimers;
  if (override.hoverShowBatteries)
    base.hoverShowBatteries = *override.hoverShowBatteries;
  if (override.hoverShowUnread)
    base.hoverShowUnread = *override.hoverShowUnread;
  if (override.hoverShowTray)
    base.hoverShowTray = *override.hoverShowTray;
  if (override.mediaGradient)
    base.mediaGradient = *override.mediaGradient;
  if (override.height)
    base.height = *override.height;
  if (override.clockSize)
    base.clockSize = *override.clockSize;
  if (override.clockSeconds)
    base.clockSeconds = *override.clockSeconds;
  if (override.clockOffset)
    base.clockOffset = *override.clockOffset;
  if (override.expandedClockOffset)
    base.expandedClockOffset = *override.expandedClockOffset;
  if (override.calendarLabels)
    base.calendarLabels = *override.calendarLabels;
  if (override.outerProgressRing)
    base.outerProgressRing = *override.outerProgressRing;
  if (override.mediaArtworkSize)
    base.mediaArtworkSize = *override.mediaArtworkSize;
  if (override.activityPriority)
    base.activityPriority = *override.activityPriority;
  if (override.splitActivities)
    base.splitActivities = *override.splitActivities;
  if (override.cycleActivities)
    base.cycleActivities = *override.cycleActivities;
  if (override.activityCycleSeconds)
    base.activityCycleSeconds = *override.activityCycleSeconds;
  if (override.hoverOpenDelayMs)
    base.hoverOpenDelayMs = *override.hoverOpenDelayMs;
  if (override.hoverCloseDelayMs)
    base.hoverCloseDelayMs = *override.hoverCloseDelayMs;
  if (override.trackPreviewSeconds)
    base.trackPreviewSeconds = *override.trackPreviewSeconds;
  if (override.pausedMediaSeconds)
    base.pausedMediaSeconds = *override.pausedMediaSeconds;
  if (override.bluetoothPreviewSeconds)
    base.bluetoothPreviewSeconds = *override.bluetoothPreviewSeconds;
  if (override.revealOnTrackChange)
    base.revealOnTrackChange = *override.revealOnTrackChange;
  if (override.trackPreviewMonitor)
    base.trackPreviewMonitor = *override.trackPreviewMonitor;
  if (override.bluetoothPreviewMonitor)
    base.bluetoothPreviewMonitor = *override.bluetoothPreviewMonitor;
  if (override.volumeBarHeight)
    base.volumeBarHeight = *override.volumeBarHeight;
  if (override.volumeShowPercentage)
    base.volumeShowPercentage = *override.volumeShowPercentage;
  return base;
}

struct BarDeadZoneOverride {
  std::optional<std::unordered_map<std::string, std::string>> actions;

  bool operator==(const BarDeadZoneOverride&) const = default;
};

struct BarMonitorOverride {
  std::optional<BarPresentation> presentation;
  IslandMonitorOverride island;
  std::string match;
  std::optional<std::string> position;
  std::optional<bool> enabled;
  std::optional<bool> autoHide;
  std::optional<bool> smartAutoHide;
  std::optional<bool> showOnWorkspaceSwitch;
  std::optional<bool> reserveSpace;
  std::optional<std::string> layer; // top | overlay
  std::optional<std::int32_t> thickness;
  std::optional<float> backgroundOpacity;
  std::optional<ColorSpec> border;
  std::optional<float> borderWidth;
  std::optional<std::int32_t> radius;
  std::optional<std::int32_t> radiusTopLeft;
  std::optional<std::int32_t> radiusTopRight;
  std::optional<std::int32_t> radiusBottomLeft;
  std::optional<std::int32_t> radiusBottomRight;
  std::optional<bool> concaveEdgeCorners;
  std::optional<std::int32_t> marginEnds;         // inset from each end of the bar along its main axis
  std::optional<std::int32_t> marginEdge;         // distance from the nearest screen edge (floats the bar when > 0)
  std::optional<std::int32_t> marginOppositeEdge; // extra reserved space on the inward side of the bar
  std::optional<std::int32_t> padding;            // main-axis padding from bar edges to start/end sections
  std::optional<std::int32_t> widgetSpacing;      // gap between widgets within a section
  std::optional<bool> shadow;                     // use the global shell shadow on this bar
  std::optional<bool> contactShadow;              // dark gradient between attached panel and bar
  std::optional<std::int32_t> panelOverlap;       // logical px the attached panel overlaps the bar edge (seam tuning)
  std::optional<float> capsuleThickness;          // capsule cross-size as a fraction of bar thickness
  std::optional<std::string> fontFamily;          // unset = inherit shell.font_family
  std::optional<float> scale;
  std::optional<float> fontScale;
  std::optional<std::vector<std::string>> startWidgets;
  std::optional<std::vector<std::string>> centerWidgets;
  std::optional<std::vector<std::string>> endWidgets;
  std::optional<bool> widgetCapsuleDefault;
  std::optional<ColorSpec> widgetCapsuleFill;
  bool widgetCapsuleBorderSpecified = false;
  std::optional<ColorSpec> widgetCapsuleBorder;
  std::optional<ColorSpec> widgetCapsuleForeground;
  std::optional<ColorSpec> widgetColor;
  std::optional<ColorSpec> widgetIconColor;
  std::optional<std::vector<BarCapsuleGroupStyle>> widgetCapsuleGroups;
  std::optional<double> widgetCapsulePadding;
  std::optional<double> widgetCapsuleRadius;
  std::optional<double> widgetCapsuleOpacity;
  std::optional<bool> hoverHighlight;
  BarDeadZoneOverride deadZone;

  [[nodiscard]] bool isAutoHideEnabled(bool baseAutoHide, bool baseSmartAutoHide) const noexcept {
    return autoHide.value_or(baseAutoHide) || smartAutoHide.value_or(baseSmartAutoHide);
  }

  bool operator==(const BarMonitorOverride&) const = default;
};

struct BarDeadZoneConfig {
  // Gesture -> action bindings for the parts of the bar no widget covers. Same grammar as widget
  // actions; see widget_action.h.
  std::unordered_map<std::string, std::string> actions;

  bool operator==(const BarDeadZoneConfig&) const = default;
};

struct BarConfig {
  BarPresentation presentation = BarPresentation::Standard;
  IslandConfig island;
  // Gesture -> action bindings applied to every widget on this bar, overriding widget-type
  // defaults and overridden in turn by `[widget.<name>.actions]`. See widget_action.h.
  std::unordered_map<std::string, std::string> actions;
  std::string name = "default";
  std::string position = "top";
  bool enabled = true;
  bool autoHide = false;             // slide out when the pointer leaves; reveal on edge approach
  bool smartAutoHide = false;        // hide while the active workspace has windows; show when it is empty
  bool showOnWorkspaceSwitch = true; // with auto_hide: briefly reveal when the active workspace changes

  [[nodiscard]] constexpr bool isAutoHideEnabled() const noexcept { return autoHide || smartAutoHide; }
  bool reserveSpace = true;  // reserve compositor exclusive zone; applies with or without auto_hide
  std::string layer = "top"; // top | overlay; attached panels use the same layer
  std::int32_t thickness = Style::barThicknessDefault;
  float backgroundOpacity = 1.0F;
  // Inside outline for the bar background; attached panels inherit the resolved values.
  ColorSpec border = colorSpecFromRole(ColorRole::Outline);
  float borderWidth = 0.0F;
  std::int32_t radius = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusTopLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusTopRight = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomRight = static_cast<std::int32_t>(Style::radiusXl);
  bool concaveEdgeCorners = true;
  std::int32_t marginEnds = 100;       // inset from each end of the bar along its main axis
  std::int32_t marginEdge = 0;         // distance from the nearest screen edge (floats the bar when > 0)
  std::int32_t marginOppositeEdge = 0; // extra reserved space on the inward side of the bar
  std::int32_t padding = 14;           // main-axis padding from bar edges to start/end sections
  std::int32_t widgetSpacing = 6;      // gap between widgets within a section
  bool shadow = true;                  // use the global shell shadow
  bool contactShadow = false;          // dark gradient between attached panel and bar
  // Logical px the attached panel overlaps the bar edge so their seam is hidden. The ideal value depends on the
  // compositor and the output's fractional scale (physical-pixel rounding), so it is exposed for per-bar/per-monitor
  // tuning. Negative values pull the panel away from the bar.
  std::int32_t panelOverlap = 1;
  float capsuleThickness = 0.76F; // capsule cross-size as a fraction of bar thickness
  float scale = 1.0F;             // content scale multiplier for glyphs and text
  float fontScale = 1.0F;         // font scale multiplier, independent of content scale
  int fontWeight = 500;           // primary label weight for bar widgets
  // Typeface for this bar's widgets; unset inherits shell.font_family. Per-widget `font_family` overrides.
  std::optional<std::string> fontFamily;
  std::vector<std::string> startWidgets = {"launcher", "wallpaper", "workspaces"};
  std::vector<std::string> centerWidgets = {"clock"};
  std::vector<std::string> endWidgets = {"media",   "tray",           "notifications", "clipboard",
                                         "network", "bluetooth",      "volume",        "brightness",
                                         "battery", "control-center", "session"};
  // When true, widgets on this bar use a capsule unless `[widget.*] capsule = false`.
  bool widgetCapsuleDefault = false;
  ColorSpec widgetCapsuleFill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // When set, bar widgets with capsules use this for icon + primary label color unless overridden per widget.
  std::optional<ColorSpec> widgetCapsuleForeground;
  // Default primary label color for all widgets on this bar (same as per-widget `color`); per-widget `color`
  // overrides.
  std::optional<ColorSpec> widgetColor;
  // Default icon color for all widgets on this bar (same as per-widget `color`); per-widget `color`
  // overrides.
  std::optional<ColorSpec> widgetIconColor;
  std::vector<BarCapsuleGroupStyle> widgetCapsuleGroups;
  // Inner padding between capsule edge and widget content (logical px), multiplied by widget content scale on the bar.
  float widgetCapsulePadding = Style::barCapsulePadding;
  // Capsule corner radius in logical pixels before content-scale; unset means automatic pill radius.
  std::optional<double> widgetCapsuleRadius;
  // Capsule background opacity multiplier (0.0–1.0).
  float widgetCapsuleOpacity = 1.0F;
  // True when `capsule_border` appears under `[bar.*]` (empty value = no outline for widgets that inherit border).
  bool widgetCapsuleBorderSpecified = false;
  std::optional<ColorSpec> widgetCapsuleBorder;
  // Soft tint of a widget's foreground color over the widget under the pointer (per member in capsule groups).
  bool hoverHighlight = true;
  BarDeadZoneConfig deadZone;
  std::vector<BarMonitorOverride> monitorOverrides;

  bool operator==(const BarConfig&) const = default;
};

struct ShortcutConfig {
  std::string type;
  bool operator==(const ShortcutConfig&) const = default;
};

enum class SessionActionButtonVariant : std::uint8_t {
  Default,
  Primary,
  Secondary,
  Destructive,
  Outline,
  Ghost,
};

struct SessionPanelActionConfig {
  // "lock" | "logout" | "suspend" | "lock_and_suspend" | "reboot" | "shutdown" | "command"
  std::string action;
  bool enabled = true;
  // When set, runs via `process::runAsync` (shell string) instead of the built-in handler.
  std::optional<std::string> command = std::nullopt;
  std::optional<std::string> label = std::nullopt;
  std::optional<std::string> glyph = std::nullopt;
  SessionActionButtonVariant variant = SessionActionButtonVariant::Default;
  std::optional<KeyChord> shortcut = std::nullopt;
  /// When > 0, the action arms a countdown (seconds) before running; activate again to confirm immediately.
  double countdownSeconds = 0.0;

  bool operator==(const SessionPanelActionConfig&) const = default;
};

struct StartupAppConfig {
  std::string id;
  std::string label;
  bool enabled = false;
  std::string kind = "app";
  std::string desktopId;
  std::string command;
  std::int32_t delaySeconds = 0;
  bool operator==(const StartupAppConfig&) const = default;
};

struct ShellSessionConfig {
  std::vector<StartupAppConfig> startupApps;
  std::vector<SessionPanelActionConfig> actions;
  // Lay the session panel actions out over multiple rows of `gridColumns` instead of
  // fitting them on a single row.
  bool grid = false;
  std::int32_t gridColumns = 3;
  bool showShortcuts = true;
  // Optional overrides for built-in session power commands. Empty = auto-detect at runtime.
  struct ShellSessionPowerConfig {
    // Shell strings run with `/bin/sh -lc` (shell=True).
    // When unset, Noctalia tries a prioritized backend list (systemd/logind/privileged helpers).
    std::optional<std::string> suspend;
    std::optional<std::string> reboot;
    std::optional<std::string> shutdown;

    bool operator==(const ShellSessionPowerConfig&) const = default;
  } power;

  bool operator==(const ShellSessionConfig&) const = default;
};

struct ShellGreeterSyncConfig {
  // Optional shell prefix before the apply helper and staging directory. Legacy
  // sync accepts the configured escalator directly. Secure sync also appends
  // --sync, so that prefix must ultimately invoke pkexec to provide PKEXEC_UID.
  // Empty selects the protocol's default escalator.
  std::string privilegeCommand;
  bool autoSync = false;

  bool operator==(const ShellGreeterSyncConfig&) const = default;
};

struct IdleBehaviorConfig {
  std::string name;
  bool enabled = true;
  double timeoutSeconds = 0.0;
  /// lock | screen_off | suspend | lock_and_suspend | command (custom shell strings)
  std::string action;
  std::string command;
  std::string resumeCommand;
  /// When `action` is `suspend`, lock the session before running suspend so lock surfaces are ready (recommended).
  bool lockBeforeSuspend = true;
  /// Shorter timeout (seconds) applied only while the session is locked; 0 = always use timeoutSeconds.
  double lockedTimeoutSeconds = 0.0;

  bool operator==(const IdleBehaviorConfig&) const = default;
};

struct NotificationFilterConfig {
  std::string name;
  bool enabled = true;
  /// Case-insensitive token matched against app name (exact/substring), desktop entry, or category.
  std::string match;
  /// Optional regular expression matched against the notification summary or body.
  std::string matchContent;
  bool showToast = true;
  bool saveHistory = true;
  bool playSound = true;
  bool bypassDnd = false;
  bool allowPermanent = true;
  std::optional<std::int32_t> overrideDuration;
  /// Empty = allow low, normal, and critical. Otherwise only listed urgencies pass this filter.
  std::vector<std::string> allowedUrgencies;

  bool operator==(const NotificationFilterConfig&) const = default;
};

struct IdleConfig {
  std::vector<IdleBehaviorConfig> behaviors;
  /// When > 0, after the compositor reports idle the shell fades a fullscreen overlay (surface color)
  /// from transparent to opaque over this many seconds, then runs `command`. Compositor activity during
  /// the fade cancels. When 0, the idle command runs immediately with no overlay.
  float preActionFadeSeconds = 2.0F;

  bool operator==(const IdleConfig&) const = default;
};

[[nodiscard]] std::vector<ShortcutConfig> defaultControlCenterShortcuts();
[[nodiscard]] std::vector<SessionPanelActionConfig> defaultSessionPanelActions();
[[nodiscard]] std::vector<IdleBehaviorConfig> defaultIdleBehaviors();

enum class IdleActionKind : std::uint8_t {
  None = 0,
  Command,
  Lock,
  ScreenOff,
  ScreenOn,
  Suspend,
  LockAndSuspend,
};

struct IdleActionRequest {
  IdleActionKind kind = IdleActionKind::None;
  std::string command;
  bool lockBeforeSuspend = true;

  bool operator==(const IdleActionRequest&) const = default;
};

struct ResolvedIdleBehavior {
  IdleActionRequest idleAction;
  IdleActionRequest resumeAction;
  std::string resumeCommand;

  bool operator==(const ResolvedIdleBehavior&) const = default;
};

void normalizeIdleBehaviorAction(IdleBehaviorConfig& behavior);
[[nodiscard]] ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior);

enum class KeybindAction : std::uint8_t {
  Validate = 0,
  Cancel = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  TabNext = 6,
  TabPrevious = 7,
  Delete = 8,
  Copy = 9,
  Save = 10,
};

[[nodiscard]] std::vector<KeyChord> defaultKeybindSet(KeybindAction action);

using ConfigOverrideValue = std::variant<
    bool, std::int64_t, double, std::string, std::vector<std::string>, std::vector<ShortcutConfig>,
    std::vector<SessionPanelActionConfig>, std::vector<IdleBehaviorConfig>, std::vector<NotificationFilterConfig>,
    std::vector<KeyChord>, std::vector<BarCapsuleGroupStyle>>;

// Optional rounded “capsule” behind a bar widget (see `[widget.*] capsule_*` in CONFIG.md).
// Corner shape, border width, and edge softness are fixed in the shell code; padding/radius are configurable.
struct WidgetBarCapsuleSpec {
  bool enabled = false;
  ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // Opaque group ID (auto-generated). Adjacent widgets in the same section with the same non-empty ID share one
  // shell and inherit the group's `BarCapsuleGroupStyle`.
  std::string group;
  // Set only when `capsule_border` is present and non-empty in config; otherwise no outline.
  std::optional<ColorSpec> border;
  // Icon + primary label color when the capsule is visible; unset = widget defaults.
  std::optional<ColorSpec> foreground;
  // Inner padding in logical pixels before content-scale (see `capsule_padding` / bar default).
  float padding = Style::barCapsulePadding;
  // Corner radius in logical pixels before content-scale; unset means automatic pill radius.
  std::optional<float> radius;
  // Capsule background opacity multiplier (0.0–1.0).
  float opacity = 1.0F;
  bool hoverHighlight = true;
  // Accordion mode (capsule groups only): collapse to the first member; hover expands.
  bool accordion = false;
  BarAccordionDirection accordionDirection = BarAccordionDirection::End;
  // Gap between group members; unset inherits the bar's widget_spacing. Meaningless for single widgets.
  std::optional<float> widgetSpacing;

  bool operator==(const WidgetBarCapsuleSpec&) const = default;
};

struct CommonWidgetOptions {
  bool enabled = true;
  bool anchor = false;
  bool interactive = true;
  float contentScale = 1.0F;
  float fontScale = 1.0F;
  std::optional<ColorSpec> color;
  std::optional<ColorSpec> iconColor;
  std::optional<std::int64_t> labelFontWeight;
  std::string labelFontFamily;
  WidgetBarCapsuleSpec capsule;
  std::string scrollRepeat = "auto";
  bool enableScroll = true;
};

struct WidgetConfig {
  std::string type; // widget type (e.g. "clock", "spacer"); defaults to the entry name
  std::unordered_map<std::string, WidgetSettingValue> settings;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tables;

  [[nodiscard]] const WidgetSettingValue* findSetting(const std::string& key) const;
  [[nodiscard]] std::string getString(const std::string& key, const std::string& fallback = {}) const;
  [[nodiscard]] std::vector<std::string>
  getStringList(const std::string& key, const std::vector<std::string>& fallback = {}) const;
  [[nodiscard]] std::int64_t getInt(const std::string& key, std::int64_t fallback = 0) const;
  [[nodiscard]] double getDouble(const std::string& key, double fallback = 0.0) const;
  [[nodiscard]] bool getBool(const std::string& key, bool fallback = false) const;
  [[nodiscard]] ColorSpec
  getColorSpec(const std::string& key, const ColorSpec& fallback, std::string_view context = {}) const;
  [[nodiscard]] std::optional<ColorSpec>
  getOptionalColorSpec(const std::string& key, std::string_view context = {}) const;
  [[nodiscard]] std::unordered_map<std::string, std::string>
  getStringMap(const std::string& key, const std::unordered_map<std::string, std::string>& fallback = {}) const;
  [[nodiscard]] bool hasSetting(const std::string& key) const;

  bool operator==(const WidgetConfig&) const = default;
};

// Merges `[bar.*]` capsule defaults with `[widget.*]` overrides (see CONFIG.md). Size/style fields such as
// `radius` are populated even when `enabled` is false so widgets can reuse capsule styling internally.
[[nodiscard]] WidgetBarCapsuleSpec resolveWidgetBarCapsuleSpec(const BarConfig& bar, const WidgetConfig* widget);
[[nodiscard]] CommonWidgetOptions resolveCommonWidgetOptions(
    const BarConfig& bar, const WidgetConfig* widget, std::string_view widgetType, float barScale
);

// Returns the group for `id` on this bar, or nullptr if `id` is empty or unregistered.
[[nodiscard]] const BarCapsuleGroupStyle* findBarCapsuleGroupStyle(const BarConfig& bar, const std::string& id);

// Builds the capsule spec a group's member widgets render with (style taken from the group).
[[nodiscard]] WidgetBarCapsuleSpec capsuleSpecFromGroup(const BarConfig& bar, const BarCapsuleGroupStyle& group);

// Group ids the scope's lanes reference. A monitor override with no capsule_group of its own reads
// the bar's array, so its lanes belong to the bar scope's reference set.
[[nodiscard]] std::set<std::string> capsuleGroupRefsForBarScope(const BarConfig& bar);
[[nodiscard]] std::set<std::string>
capsuleGroupRefsForMonitorScope(const BarConfig& bar, const BarMonitorOverride& monitorOverride);

// Rebuilds an overriding capsule_group array against the config-file array, in file order: an
// overridden group keeps its edited style, a file group the lanes reference again comes back, and a
// GUI-created group survives only while a lane still references it (nothing can reach it otherwise).
[[nodiscard]] std::vector<BarCapsuleGroupStyle> reconcileCapsuleGroups(
    const std::vector<BarCapsuleGroupStyle>& current, const std::vector<BarCapsuleGroupStyle>& base,
    const std::set<std::string>& referenced
);
[[nodiscard]] float
resolveWidgetContentScale(float barScale, const WidgetConfig* widget, std::string_view context = "widget.scale");
[[nodiscard]] float
resolveWidgetFontScale(float barScale, const WidgetConfig* widget, std::string_view context = "widget");

// Shared output selector matching used by monitor-scoped config and IPC selectors.
// Matches connector name exactly, or a word-boundary token within output description.
[[nodiscard]] bool outputMatchesSelector(const std::string& match, const WaylandOutput& output);

enum class WallpaperFillMode : std::uint8_t {
  Center = 0,
  Crop = 1,
  Fit = 2,
  Stretch = 3,
  Repeat = 4,
  Span = 5,
};

enum class WallpaperTransition : std::uint8_t {
  Fade = 0,
  Wipe = 1,
  Disc = 2,
  Stripes = 3,
  Zoom = 4,
  Honeycomb = 5,
};

enum class LockscreenTransition : std::uint8_t {
  Fade = 0,
  Wipe = 1,
  Disc = 2,
  Stripes = 3,
  Zoom = 4,
  Honeycomb = 5,
};

struct WallpaperMonitorOverride {
  std::string match;
  std::optional<bool> enabled;
  std::optional<ColorSpec> fillColor;
  std::optional<std::string> directory;
  std::optional<std::string> directoryLight;
  std::optional<std::string> directoryDark;

  bool operator==(const WallpaperMonitorOverride&) const = default;
};

struct WallpaperAutomationConfig {
  enum class Order : std::uint8_t {
    Random = 0,
    Alphabetical = 1,
  };

  bool enabled = false;
  std::int32_t intervalSeconds = 1800;
  Order order = Order::Random;
  bool recursive = false;

  bool operator==(const WallpaperAutomationConfig&) const = default;
};

struct WallpaperConfig {
  bool enabled = true;
  WallpaperFillMode fillMode = WallpaperFillMode::Crop;
  std::optional<ColorSpec> fillColor;
  std::vector<WallpaperTransition> transitions = {WallpaperTransition::Fade, WallpaperTransition::Wipe,
                                                  WallpaperTransition::Disc, WallpaperTransition::Stripes,
                                                  WallpaperTransition::Zoom, WallpaperTransition::Honeycomb};
  float transitionDurationMs = 1500.0F;
  float edgeSmoothness = 0.3F;
  bool transitionOnStartup = false;
  std::string directory;      // empty = XDG_PICTURES_DIR
  std::string directoryLight; // empty = directory
  std::string directoryDark;  // empty = directory
  bool perMonitorDirectories = false;
  WallpaperAutomationConfig automation;
  std::vector<WallpaperMonitorOverride> monitorOverrides;

  bool operator==(const WallpaperConfig&) const = default;
};

struct BackdropConfig {
  bool enabled = false;
  float blurIntensity = 0.5F;
  float tintIntensity = 0.3F;

  bool operator==(const BackdropConfig&) const = default;
};

struct LockscreenConfig {
  bool enabled = true;
  // Lock on PrepareForSleep (lid close / systemctl suspend) via logind sleep-delay inhibit.
  // Distinct from idle/session lock_and_suspend actions.
  bool lockBeforeSuspend = true;
  bool fingerprint = true;
  bool allowEmptyPassword = false;
  bool blurredDesktop = false;
  std::vector<LockscreenTransition> transitions = {LockscreenTransition::Fade, LockscreenTransition::Wipe,
                                                   LockscreenTransition::Disc, LockscreenTransition::Stripes,
                                                   LockscreenTransition::Zoom, LockscreenTransition::Honeycomb};
  float transitionDurationMs = 1500.0F;
  float edgeSmoothness = 0.3F;
  float blurIntensity = 0.5F;
  float tintIntensity = 0.3F;
  std::string wallpaper;
  std::vector<std::string> monitors;

  bool operator==(const LockscreenConfig&) const = default;
};

[[nodiscard]] inline bool isLockScreenEnabled(const LockscreenConfig& lockscreen) noexcept {
  return lockscreen.enabled;
}

[[nodiscard]] inline bool shouldLockBeforeSuspend(const LockscreenConfig& lockscreen) noexcept {
  return lockscreen.enabled && lockscreen.lockBeforeSuspend;
}

template <typename T> struct EnumOption {
  T value;
  std::string_view key;
  std::string_view labelKey;
};

template <typename T, std::size_t N>
constexpr std::optional<T> enumFromKey(const EnumOption<T> (&options)[N], std::string_view key) {
  for (const auto& opt : options) {
    if (opt.key == key) {
      return opt.value;
    }
  }
  return std::nullopt;
}

template <typename T, std::size_t N> constexpr std::string_view enumToKey(const EnumOption<T> (&options)[N], T value) {
  for (const auto& opt : options) {
    if (opt.value == value) {
      return opt.key;
    }
  }
  return {};
}

constexpr EnumOption<IslandActivityPriority> kIslandActivityPriority[] = {
    {IslandActivityPriority::TimersDownloadsMedia, "timers-downloads-media",
     "settings.options.island.timers-downloads-media"},
    {IslandActivityPriority::TimersMediaDownloads, "timers-media-downloads",
     "settings.options.island.timers-media-downloads"},
    {IslandActivityPriority::DownloadsTimersMedia, "downloads-timers-media",
     "settings.options.island.downloads-timers-media"},
    {IslandActivityPriority::DownloadsMediaTimers, "downloads-media-timers",
     "settings.options.island.downloads-media-timers"},
    {IslandActivityPriority::MediaTimersDownloads, "media-timers-downloads",
     "settings.options.island.media-timers-downloads"},
    {IslandActivityPriority::MediaDownloadsTimers, "media-downloads-timers",
     "settings.options.island.media-downloads-timers"},
};
constexpr EnumOption<IslandAppearance> kIslandAppearances[] = {
    {IslandAppearance::Cupertino, "cupertino", "settings.options.island.appearance-cupertino"},
    {IslandAppearance::Theme, "theme", "settings.options.island.appearance-theme"},
};
constexpr EnumOption<IslandCalendarLabels> kIslandCalendarLabels[] = {
    {IslandCalendarLabels::Initials, "initials", "settings.options.island.calendar-initials"},
    {IslandCalendarLabels::TodayAbbreviated, "today", "settings.options.island.calendar-today"},
    {IslandCalendarLabels::Abbreviated, "abbreviated", "settings.options.island.calendar-abbreviated"},
};
constexpr EnumOption<BarPresentation> kBarPresentations[] = {
    {BarPresentation::Standard, "bar", "settings.options.bar.standard"},
    {BarPresentation::Island, "island", "settings.options.bar.island"},
};
constexpr EnumOption<BarAccordionDirection> kBarAccordionDirections[] = {
    {BarAccordionDirection::End, "end", "settings.options.accordion-direction.end"},
    {BarAccordionDirection::Start, "start", "settings.options.accordion-direction.start"},
};

enum class DockEdge : std::uint8_t {
  Top = 0,
  Bottom = 1,
  Left = 2,
  Right = 3,
};

constexpr EnumOption<DockEdge> kDockEdges[] = {
    {DockEdge::Top, "top", "settings.options.edge.top"},
    {DockEdge::Bottom, "bottom", "settings.options.edge.bottom"},
    {DockEdge::Left, "left", "settings.options.edge.left"},
    {DockEdge::Right, "right", "settings.options.edge.right"},
};

enum class DockLauncherPosition : std::uint8_t {
  None = 0,
  Start = 1,
  End = 2,
};

constexpr EnumOption<DockLauncherPosition> kDockLauncherPositions[] = {
    {DockLauncherPosition::None, "none", "settings.options.dock-launcher-position.none"},
    {DockLauncherPosition::Start, "start", "settings.options.dock-launcher-position.start"},
    {DockLauncherPosition::End, "end", "settings.options.dock-launcher-position.end"},
};

struct DockConfig {
  bool enabled = false; // opt-in; dock is hidden by default
  DockEdge position = DockEdge::Bottom;
  bool activeMonitorOnly = false;    // render only on preferred active output
  std::int32_t iconSize = 48;        // icon size in pixels (before ui_scale)
  std::int32_t mainAxisPadding = 4;  // inner padding along the icon row (main axis)
  std::int32_t crossAxisPadding = 4; // inner padding perpendicular to the icon row
  std::int32_t itemSpacing = 2;      // gap between items
  float backgroundOpacity = 0.88F;
  // Inside outline for the dock background.
  ColorSpec border = colorSpecFromRole(ColorRole::Outline);
  float borderWidth = 0.0F;
  // Faint hairline around the dock's glass, as on macOS; drawn when border_width is 0.
  bool hairlineBorder = true;
  // Defaults float a fully rounded dock above the screen edge, like macOS.
  std::int32_t radius = static_cast<std::int32_t>(Style::radiusXl); // dock background corner radius
  std::int32_t radiusTopLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusTopRight = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomRight = static_cast<std::int32_t>(Style::radiusXl);
  // Distance from the nearest screen edge (floats the dock when > 0).
  std::int32_t marginEdge = static_cast<std::int32_t>(Style::spaceSm);
  bool concaveEdgeCorners = false; // carve concave corners on the side that touches the screen edge
  std::int32_t marginEnds = 0;     // inset from each end of the dock along its main axis
  bool shadow = true;              // use the global shell shadow
  bool showRunning = true;         // also show running apps not in pinned list
  bool autoHide = false;           // slide out when not hovered (overlay mode)
  bool smartAutoHide = false;      // hide while the active workspace has windows; show when it is empty
  std::string layer = "top";       // top | overlay

  [[nodiscard]] constexpr bool isAutoHideEnabled() const noexcept { return autoHide || smartAutoHide; }
  bool reserveSpace = true;          // reserve compositor exclusive zone; applies with or without auto_hide
  float activeScale = 1.0F;          // focused app icon scale
  float inactiveScale = 1.0F;        // non-focused app icon scale
  bool magnification = true;         // magnify icons near the pointer (macOS-style)
  float magnificationScale = 1.45F;  // max icon scale multiplier at the pointer center
  bool animateLaunch = true;         // brief icon bounce when launching an app
  bool windowPreviews = false;       // optional on-demand window picker on hover
  std::int32_t previewDelayMs = 450; // delay before opening window previews
  std::int32_t hideDelayMs = 200;    // grace period after leaving the dock
  float activeOpacity = 1.0F;        // focused app icon opacity
  float inactiveOpacity = 1.0F;      // non-focused app icon opacity
  bool showDots = true;              // show a dot below apps that are running
  bool showInstanceCount = false;    // show a badge with count when app has >1 window
  bool showBadges = true;            // red unread-count badges that apps publish (LauncherEntry)
  DockLauncherPosition launcherPosition = DockLauncherPosition::None;
  std::string launcherIcon = "grid-dots";   // Tabler glyph name
  std::string launcherCustomImage = "";     // image path; overrides launcherIcon glyph when set
  bool launcherCustomImageColorize = false; // tint the custom image with the icon color role
  std::vector<std::string> pinned;          // desktop entry IDs to always show
  std::vector<std::string> monitors;        // connector names to show on; empty = all outputs
  bool operator==(const DockConfig&) const = default;
};

struct DesktopWidgetsGridState {
  bool visible = true;
  std::int32_t cellSize = 16;
  std::int32_t majorInterval = 4;

  bool operator==(const DesktopWidgetsGridState&) const = default;
};

struct DesktopWidgetState {
  std::string id;
  std::string type = "clock";
  std::string outputName;
  float cx = 0.0F;
  float cy = 0.0F;
  // Logical output size the position was last stored against. Zero denotes a
  // legacy position whose reference size has not been recorded yet.
  float placementWidth = 0.0F;
  float placementHeight = 0.0F;
  // Box size of the widget's grid tile, in logical px. 0 means "unsized": the tile
  // auto-fits the content's natural size. Resizing in the editor sets explicit values.
  float boxWidth = 0.0F;
  float boxHeight = 0.0F;
  float rotationRad = 0.0F;
  bool flipX = false;
  bool flipY = false;
  bool enabled = true;
  std::unordered_map<std::string, WidgetSettingValue> settings;

  bool operator==(const DesktopWidgetState&) const = default;
};

struct DesktopWidgetsConfig {
  bool enabled = true;
  std::int32_t schemaVersion = 2;
  DesktopWidgetsGridState grid;
  std::vector<DesktopWidgetState> widgets;

  bool operator==(const DesktopWidgetsConfig&) const = default;
};

struct LockscreenWidgetsConfig {
  bool enabled = false;
  std::int32_t schemaVersion = 2;
  DesktopWidgetsGridState grid;
  std::vector<DesktopWidgetState> widgets;

  bool operator==(const LockscreenWidgetsConfig&) const = default;
};

struct OsdKindsConfig {
  bool volume = true;
  bool volumeOutput = true;
  bool volumeInput = true;
  bool brightness = true;
  bool wifi = true;
  bool bluetooth = true;
  bool powerProfile = true;
  bool caffeine = true;
  bool nightlight = true;
  bool dnd = true;
  bool lockKeys = true;
  bool keyboardLayout = true;
  bool media = true;
  bool privacy = true;
  bool keyboardBacklight = true;
  bool charging = true;
  bool operator==(const OsdKindsConfig&) const = default;
};

struct OsdConfig {
  bool enabled = true; // master gate for all OSD popups
  std::string position = "top_center";
  std::string positionVertical = "top_center";
  std::string orientation = "horizontal";
  float scale = 1.0F;
  float backgroundOpacity = 0.97F;
  bool border = true; // outline around OSD popup cards
  int offsetX = 20;
  int offsetY = 8;
  std::vector<std::string> monitors;
  OsdKindsConfig kinds;

  bool operator==(const OsdConfig&) const = default;
};

struct NotificationConfig {
  bool enableDaemon = true;
  bool showAppName = true;
  bool showActions = true;
  std::string position = "top_right";
  std::string layer = "top"; // top | overlay
  float scale = 1.0F;
  float backgroundOpacity = 0.97F; // toast card background alpha (0.0–1.0)
  bool border = true;              // outline around toast cards
  int offsetX = 20;                // absolute horizontal margin from the screen edge
  int offsetY = 8;                 // absolute vertical margin from the screen edge
  std::vector<std::string> monitors;
  bool collapseOnDismiss = true;
  bool keepDismissedInHistory = true;
  int historyRetentionHours = 0;
  int maxVisible = 0; // 0 = unlimited (space-based only)

  std::vector<NotificationFilterConfig> filters;

  bool operator==(const NotificationConfig&) const = default;
};

constexpr EnumOption<SessionActionButtonVariant> kSessionActionButtonVariants[] = {
    {SessionActionButtonVariant::Default, "default", "settings.session-actions.variant.default"},
    {SessionActionButtonVariant::Primary, "primary", "settings.session-actions.variant.primary"},
    {SessionActionButtonVariant::Secondary, "secondary", "settings.session-actions.variant.secondary"},
    {SessionActionButtonVariant::Destructive, "destructive", "settings.session-actions.variant.destructive"},
    {SessionActionButtonVariant::Outline, "outline", "settings.session-actions.variant.outline"},
    {SessionActionButtonVariant::Ghost, "ghost", "settings.session-actions.variant.ghost"},
};

enum class ClipboardAutoPasteMode : std::uint8_t {
  Off = 0,
  Auto = 1,
  CtrlV = 2,
  CtrlShiftV = 3,
  ShiftInsert = 4,
};

constexpr EnumOption<ClipboardAutoPasteMode> kClipboardAutoPasteModes[] = {
    {ClipboardAutoPasteMode::Off, "off", "common.states.off"},
    {ClipboardAutoPasteMode::Auto, "auto", "common.states.auto"},
    {ClipboardAutoPasteMode::CtrlV, "ctrl_v", "settings.options.clipboard.auto-paste.ctrl-v"},
    {ClipboardAutoPasteMode::CtrlShiftV, "ctrl_shift_v", "settings.options.clipboard.auto-paste.ctrl-shift-v"},
    {ClipboardAutoPasteMode::ShiftInsert, "shift_insert", "settings.options.clipboard.auto-paste.shift-insert"},
};

enum class StorageKeySource : std::uint8_t {
  SecretService = 0,
  File = 1,
};

enum class PasswordMaskStyle : std::uint8_t {
  CircleFilled = 0,
  RandomIcons = 1,
};

constexpr EnumOption<PasswordMaskStyle> kPasswordMaskStyles[] = {
    {PasswordMaskStyle::CircleFilled, "default", "settings.options.shell.password-style.filled-circles"},
    {PasswordMaskStyle::RandomIcons, "random", "settings.options.shell.password-style.random-icons"},
};

enum class ShadowDirection : std::uint8_t {
  Center = 0,
  Down = 1,
  Up = 2,
  Left = 3,
  Right = 4,
  DownLeft = 5,
  DownRight = 6,
  UpLeft = 7,
  UpRight = 8,
};

constexpr EnumOption<ShadowDirection> kShadowDirections[] = {
    {ShadowDirection::Center, "center", "settings.options.shell.shadow-direction.center"},
    {ShadowDirection::Down, "down", "settings.options.shell.shadow-direction.down"},
    {ShadowDirection::Up, "up", "settings.options.shell.shadow-direction.up"},
    {ShadowDirection::Left, "left", "settings.options.shell.shadow-direction.left"},
    {ShadowDirection::Right, "right", "settings.options.shell.shadow-direction.right"},
    {ShadowDirection::DownLeft, "down_left", "settings.options.shell.shadow-direction.down-left"},
    {ShadowDirection::DownRight, "down_right", "settings.options.shell.shadow-direction.down-right"},
    {ShadowDirection::UpLeft, "up_left", "settings.options.shell.shadow-direction.up-left"},
    {ShadowDirection::UpRight, "up_right", "settings.options.shell.shadow-direction.up-right"},
};

struct ShadowDirectionOffset {
  std::int32_t x;
  std::int32_t y;
};

constexpr ShadowDirectionOffset shadowDirectionOffset(ShadowDirection dir) noexcept {
  // Cast distance in logical px; with the larger blur this reads as a lifted macOS surface.
  constexpr std::int32_t kShadowOffset = 6;
  switch (dir) {
  case ShadowDirection::Center:
    return {0, 0};
  case ShadowDirection::Down:
    return {0, kShadowOffset};
  case ShadowDirection::Up:
    return {0, -kShadowOffset};
  case ShadowDirection::Left:
    return {-kShadowOffset, 0};
  case ShadowDirection::Right:
    return {kShadowOffset, 0};
  case ShadowDirection::DownLeft:
    return {-kShadowOffset, kShadowOffset};
  case ShadowDirection::DownRight:
    return {kShadowOffset, kShadowOffset};
  case ShadowDirection::UpLeft:
    return {-kShadowOffset, -kShadowOffset};
  case ShadowDirection::UpRight:
    return {kShadowOffset, -kShadowOffset};
  }
  return {0, kShadowOffset};
}

enum class PanelTransparencyMode : std::uint8_t {
  Solid = 0,
  Soft = 1,
  Glass = 2,
  Auto = 3, // Glass when the compositor offers background blur, otherwise Solid
};

constexpr EnumOption<PanelTransparencyMode> kPanelTransparencyModes[] = {
    {PanelTransparencyMode::Auto, "auto", "settings.options.shell.panel-transparency.auto"},
    {PanelTransparencyMode::Solid, "solid", "settings.options.shell.panel-transparency.solid"},
    {PanelTransparencyMode::Soft, "soft", "settings.options.shell.panel-transparency.soft"},
    {PanelTransparencyMode::Glass, "glass", "settings.options.shell.panel-transparency.glass"},
};

// Expects a resolved mode (see ui::material::resolveMode); Auto is treated as Solid.
[[nodiscard]] float
panelCardOpacityForTransparencyMode(PanelTransparencyMode mode, float panelBackgroundOpacity) noexcept;

enum class PanelPlacement : std::uint8_t {
  Attached = 0,
  Floating = 1,
};

constexpr EnumOption<PanelPlacement> kPanelPlacements[] = {
    {PanelPlacement::Attached, "attached", "settings.options.shell.panel-placement.attached"},
    {PanelPlacement::Floating, "floating", "settings.options.shell.panel-placement.floating"},
};

// Screen-anchor tokens for a floating panel's `<panel>_position`. "auto" keeps the
// panel bar-relative (the historical floating behavior); "center" reserves the
// screen center; the rest anchor to a screen edge/corner. Same vocabulary as the
// OSD/notification `position`.
constexpr std::string_view kPanelPositions[] = {"auto",          "center",      "top_left",     "top_center",
                                                "top_right",     "center_left", "center_right", "bottom_left",
                                                "bottom_center", "bottom_right"};

constexpr EnumOption<WallpaperFillMode> kWallpaperFillModes[] = {
    {WallpaperFillMode::Center, "center", "settings.options.wallpaper.fill.center"},
    {WallpaperFillMode::Crop, "crop", "settings.options.wallpaper.fill.crop"},
    {WallpaperFillMode::Fit, "fit", "settings.options.wallpaper.fill.fit"},
    {WallpaperFillMode::Stretch, "stretch", "settings.options.wallpaper.fill.stretch"},
    {WallpaperFillMode::Repeat, "repeat", "settings.options.wallpaper.fill.repeat"},
    {WallpaperFillMode::Span, "span", "settings.options.wallpaper.fill.span"},
};

constexpr EnumOption<WallpaperAutomationConfig::Order> kWallpaperAutomationOrders[] = {
    {WallpaperAutomationConfig::Order::Random, "random", "settings.options.wallpaper.order.random"},
    {WallpaperAutomationConfig::Order::Alphabetical, "alphabetical", "settings.options.wallpaper.order.alphabetical"},
};

constexpr EnumOption<WallpaperTransition> kWallpaperTransitions[] = {
    {WallpaperTransition::Disc, "disc", "settings.options.wallpaper.transition.disc"},
    {WallpaperTransition::Fade, "fade", "settings.options.wallpaper.transition.fade"},
    {WallpaperTransition::Honeycomb, "honeycomb", "settings.options.wallpaper.transition.honeycomb"},
    {WallpaperTransition::Stripes, "stripes", "settings.options.wallpaper.transition.stripes"},
    {WallpaperTransition::Wipe, "wipe", "settings.options.wallpaper.transition.wipe"},
    {WallpaperTransition::Zoom, "zoom", "settings.options.wallpaper.transition.zoom"},
};

constexpr EnumOption<LockscreenTransition> kLockscreenTransitions[] = {
    {LockscreenTransition::Disc, "disc", "settings.options.lockscreen.transition.disc"},
    {LockscreenTransition::Fade, "fade", "settings.options.lockscreen.transition.fade"},
    {LockscreenTransition::Honeycomb, "honeycomb", "settings.options.lockscreen.transition.honeycomb"},
    {LockscreenTransition::Stripes, "stripes", "settings.options.lockscreen.transition.stripes"},
    {LockscreenTransition::Wipe, "wipe", "settings.options.lockscreen.transition.wipe"},
    {LockscreenTransition::Zoom, "zoom", "settings.options.lockscreen.transition.zoom"},
};

// One config-driven dmenu-style launcher entry. The provider runs `command`, splits
// its stdout into newline-separated candidates, and on activation either runs `exec`
// (with {selection}/{query} substituted) or copies the selection to the clipboard.
struct DmenuEntryConfig {
  // Canonical flat identifier; the [shell.launcher.dmenu.entry.<id>] table key. Used as
  // the provider id suffix (dmenu.<id>) and the usage-tracking key.
  std::string id;
  // Shell string run via /bin/sh -lc; stdout lines become candidates. A tab in a line
  // splits it into title \t description (the raw line is still the selection value).
  std::string command;
  // When set, the activated line is substituted into {selection} and run detached.
  // When unset, the selection is copied to the clipboard.
  std::optional<std::string> exec;
  // Launcher trigger word (e.g. "ssh"), combined with shell.launcher.provider_prefix like
  // the built-in providers (-> "/ssh"). Empty leaves the entry reachable only via
  // global = true, otherwise it is unreachable (surfaced as a config warning).
  std::optional<std::string> prefix;
  std::optional<std::string> label; // Provider overview title; defaults to the id.
  std::optional<std::string> glyph; // Tabler glyph name; defaults to "terminal".
  bool global = false;              // Include results in non-prefixed search.
  bool freeform = false;            // Let typed query text become an activatable result.

  bool operator==(const DmenuEntryConfig&) const = default;
};

// A launcher quicklink: a URL opened by name, or a search URL whose {query} the launcher fills in
// from the text typed after its keyword (e.g. "gh noctalia").
struct LauncherQuicklinkConfig {
  std::string id; // [shell.launcher.quicklinks.<id>] table key
  std::string name;
  std::string url;
  std::string keyword;
  std::string glyph;

  bool operator==(const LauncherQuicklinkConfig&) const = default;
};

// A launcher snippet: saved text pasted by name or keyword. {date}, {time}, {datetime} and
// {clipboard} are expanded when it is pasted.
struct LauncherSnippetConfig {
  std::string id; // [shell.launcher.snippets.<id>] table key
  std::string name;
  std::string keyword;
  std::string text;

  bool operator==(const LauncherSnippetConfig&) const = default;
};

struct LauncherProviderConfig {
  std::string name;
  std::string prefix;
  std::optional<bool> global;

  bool operator==(const LauncherProviderConfig&) const = default;
};

enum class HyprlandAnimationEasing : std::uint8_t { Smooth, Snappy, Gentle, Linear, Custom, Spring, Inherit };
enum class HyprlandWindowAnimation : std::uint8_t { Pop, Slide, Fade, Instant };
enum class HyprlandWorkspaceAnimation : std::uint8_t { Slide, Vertical, Fade, Instant };

constexpr EnumOption<HyprlandAnimationEasing> kHyprlandAnimationEasings[] = {
    {HyprlandAnimationEasing::Custom, "custom", "settings.schema.hyprland-appearance.easing-custom"},
    {HyprlandAnimationEasing::Spring, "spring", "settings.schema.hyprland-appearance.easing-spring"},
    {HyprlandAnimationEasing::Inherit, "inherit", "settings.schema.hyprland-appearance.easing-inherit"},
    {HyprlandAnimationEasing::Smooth, "smooth", "settings.schema.hyprland-appearance.easing-smooth"},
    {HyprlandAnimationEasing::Snappy, "snappy", "settings.schema.hyprland-appearance.easing-snappy"},
    {HyprlandAnimationEasing::Gentle, "gentle", "settings.schema.hyprland-appearance.easing-gentle"},
    {HyprlandAnimationEasing::Linear, "linear", "settings.schema.hyprland-appearance.easing-linear"},
};
constexpr EnumOption<HyprlandWindowAnimation> kHyprlandWindowAnimations[] = {
    {HyprlandWindowAnimation::Pop, "pop", "settings.schema.hyprland-appearance.style-pop"},
    {HyprlandWindowAnimation::Slide, "slide", "settings.schema.hyprland-appearance.style-slide"},
    {HyprlandWindowAnimation::Fade, "fade", "settings.schema.hyprland-appearance.style-fade"},
    {HyprlandWindowAnimation::Instant, "instant", "settings.schema.hyprland-appearance.style-instant"},
};
constexpr EnumOption<HyprlandWorkspaceAnimation> kHyprlandWorkspaceAnimations[] = {
    {HyprlandWorkspaceAnimation::Slide, "slide", "settings.schema.hyprland-appearance.style-horizontal"},
    {HyprlandWorkspaceAnimation::Vertical, "vertical", "settings.schema.hyprland-appearance.style-vertical"},
    {HyprlandWorkspaceAnimation::Fade, "fade", "settings.schema.hyprland-appearance.style-fade"},
    {HyprlandWorkspaceAnimation::Instant, "instant", "settings.schema.hyprland-appearance.style-instant"},
};

struct HyprlandMotionCurve {
  HyprlandAnimationEasing easing = HyprlandAnimationEasing::Inherit;
  float x1 = 0.22F, y1 = 1.0F, x2 = 0.36F, y2 = 1.0F;
  float stiffness = 200.0F, damping = 20.0F;
  bool operator==(const HyprlandMotionCurve&) const = default;
};

struct HyprlandAppearanceConfig {
  bool enabled = false;
  bool followTheme = false;
  int gapsIn = 6;
  int gapsOut = 12;
  int borderSize = 2;
  int rounding = 12;
  float activeOpacity = 1.0F;
  float inactiveOpacity = 1.0F;
  bool blurEnabled = true;
  int blurSize = 3;
  int blurPasses = 2;
  bool blurFocusManaged = false;
  float blurBrightness = 1.0F, blurContrast = 0.8916F, blurVibrancy = 0.1696F, blurNoise = 0.0117F;
  bool blurPopups = false, blurSpecial = false;
  float blurPopupsIgnorealpha = 0.2F;
  bool dimInactive = false;
  float dimStrength = 0.5F, dimSpecial = 0.2F;
  float fullscreenOpacity = 1.0F, roundingPower = 2.0F;
  bool shadowEnabled = true;
  int shadowRange = 4;
  bool decorationEffectsManaged = false;
  int shadowPower = 3;
  bool shadowSharp = false;
  int shadowOffsetX = 0, shadowOffsetY = 0;
  float shadowScale = 1.0F;
  std::optional<ColorSpec> shadowColor = fixedColorSpec(hex("#1a1a1a"));
  std::optional<ColorSpec> shadowInactiveColor;
  float shadowOpacity = 0.93F, shadowInactiveOpacity = 0.93F;
  bool glowEnabled = false;
  int glowRange = 10, glowPower = 3;
  std::optional<ColorSpec> glowColor = colorSpecFromRole(ColorRole::Primary);
  std::optional<ColorSpec> glowInactiveColor;
  float glowOpacity = 0.35F, glowInactiveOpacity = 0.12F;
  bool animationsEnabled = true;
  bool customAnimations = false;
  float animationSpeed = 1.0F;
  HyprlandAnimationEasing animationEasing = HyprlandAnimationEasing::Smooth;
  HyprlandWindowAnimation windowAnimation = HyprlandWindowAnimation::Pop;
  HyprlandWorkspaceAnimation workspaceAnimation = HyprlandWorkspaceAnimation::Slide;

  HyprlandMotionCurve curve, openingCurve, closingCurve, movingCurve, workspaceCurve;
  float openingDuration = 400.0F, closingDuration = 250.0F, movingDuration = 300.0F, workspaceDuration = 400.0F;
  bool glassManaged = false, glassEnabled = true, glassLight = false, glassLayers = false;
  float glassBlur = 1.5F, glassRefraction = 0.35F, glassChromatic = 0.15F, glassLens = 0.2F;
  float glassOpacity = 1.0F, glassFresnel = 0.6F, glassSpecular = 0.8F;
  bool cursorManaged = false, cursorEnabled = true, cursorStretch = false, cursorShake = true;
  float cursorShakeLimit = 6.0F;
  bool overviewManaged = false, overviewBottom = true, overviewHideLayers = false;
  int overviewHeight = 220;
  bool overviewStyleManaged = false, overviewCentered = true, overviewBlur = true;
  int overviewMargin = 12, overviewPanelBorder = 1, overviewWorkspaceBorder = 2;
  float overviewDragOpacity = 0.85F, overviewDuration = 350;
  std::optional<ColorSpec> overviewPanelColor = colorSpecFromRole(ColorRole::Surface);
  std::optional<ColorSpec> overviewPanelBorderColor = colorSpecFromRole(ColorRole::Primary);
  std::optional<ColorSpec> overviewActiveBackground = colorSpecFromRole(ColorRole::Primary);
  std::optional<ColorSpec> overviewInactiveBackground = colorSpecFromRole(ColorRole::Surface);
  std::optional<ColorSpec> overviewActiveBorder = colorSpecFromRole(ColorRole::Primary);
  std::optional<ColorSpec> overviewInactiveBorder = colorSpecFromRole(ColorRole::Primary);
  float overviewPanelOpacity = .82F, overviewPanelBorderOpacity = .25F;
  float overviewActiveBackgroundOpacity = .12F, overviewInactiveBackgroundOpacity = .45F;
  float overviewActiveBorderOpacity = .85F, overviewInactiveBorderOpacity = .12F;
  bool operator==(const HyprlandAppearanceConfig&) const = default;
};

enum class HyprlandRuleSwitch : std::uint8_t { Inherit, On, Off };
constexpr EnumOption<HyprlandRuleSwitch> kHyprlandRuleSwitches[] = {
    {HyprlandRuleSwitch::Inherit, "inherit", "settings.hyprland-editor.rule-inherit"},
    {HyprlandRuleSwitch::On, "on", "settings.hyprland-editor.rule-on"},
    {HyprlandRuleSwitch::Off, "off", "settings.hyprland-editor.rule-off"},
};
enum class HyprlandRuleScope : std::uint8_t { All, Floating, Fullscreen };
constexpr EnumOption<HyprlandRuleScope> kHyprlandRuleScopes[] = {
    {HyprlandRuleScope::All, "all", "settings.hyprland-editor.rule-all"},
    {HyprlandRuleScope::Floating, "floating", "settings.hyprland-editor.rule-floating"},
    {HyprlandRuleScope::Fullscreen, "fullscreen", "settings.hyprland-editor.rule-fullscreen"},
};
struct HyprlandAppRule {
  std::string name, appClass;
  bool enabled = true, opacityManaged = false, roundingManaged = false;
  HyprlandRuleScope scope = HyprlandRuleScope::All;
  float activeOpacity = 1, inactiveOpacity = 1, fullscreenOpacity = 1;
  int rounding = 16;
  HyprlandRuleSwitch blur = HyprlandRuleSwitch::Inherit, shadow = HyprlandRuleSwitch::Inherit;
  HyprlandRuleSwitch dim = HyprlandRuleSwitch::Inherit, animations = HyprlandRuleSwitch::Inherit;
  HyprlandRuleSwitch glass = HyprlandRuleSwitch::Inherit;
  bool operator==(const HyprlandAppRule&) const = default;
};

enum class HyprlandPlacementMode : std::uint8_t { Inherit, Floating, Tiled };
constexpr EnumOption<HyprlandPlacementMode> kHyprlandPlacementModes[] = {
    {HyprlandPlacementMode::Inherit, "inherit", "settings.hyprland-placement.kHyprlandPlacementModes-inherit"},
    {HyprlandPlacementMode::Floating, "floating", "settings.hyprland-placement.kHyprlandPlacementModes-floating"},
    {HyprlandPlacementMode::Tiled, "tiled", "settings.hyprland-placement.kHyprlandPlacementModes-tiled"},
};
enum class HyprlandPlacementWorkspace : std::uint8_t { Inherit, Number, Named, Special };
constexpr EnumOption<HyprlandPlacementWorkspace> kHyprlandPlacementWorkspaces[] = {
    {HyprlandPlacementWorkspace::Inherit, "inherit",
     "settings.hyprland-placement.kHyprlandPlacementWorkspaces-inherit"},
    {HyprlandPlacementWorkspace::Number, "number", "settings.hyprland-placement.kHyprlandPlacementWorkspaces-number"},
    {HyprlandPlacementWorkspace::Named, "named", "settings.hyprland-placement.kHyprlandPlacementWorkspaces-named"},
    {HyprlandPlacementWorkspace::Special, "special",
     "settings.hyprland-placement.kHyprlandPlacementWorkspaces-special"},
};
enum class HyprlandPlacementPosition : std::uint8_t { Inherit, Center, Offset };
constexpr EnumOption<HyprlandPlacementPosition> kHyprlandPlacementPositions[] = {
    {HyprlandPlacementPosition::Inherit, "inherit", "settings.hyprland-placement.kHyprlandPlacementPositions-inherit"},
    {HyprlandPlacementPosition::Center, "center", "settings.hyprland-placement.kHyprlandPlacementPositions-center"},
    {HyprlandPlacementPosition::Offset, "offset", "settings.hyprland-placement.kHyprlandPlacementPositions-offset"},
};
struct HyprlandPlacementRule {
  std::string name, appClass;
  bool enabled = false;
  HyprlandPlacementMode mode = HyprlandPlacementMode::Inherit;
  HyprlandPlacementWorkspace workspace = HyprlandPlacementWorkspace::Inherit;
  int workspaceNumber = 1;
  std::string workspaceName;
  bool workspaceSilent = true;
  bool sizeManaged = false;
  int width = 960, height = 640;
  HyprlandPlacementPosition position = HyprlandPlacementPosition::Inherit;
  int x = 0, y = 0;
  HyprlandRuleSwitch pin = HyprlandRuleSwitch::Inherit;
  bool operator==(const HyprlandPlacementRule&) const = default;
};

struct HyprlandProfileSwitchingConfig {
  bool enabled = false;
  std::string lightProfile = "@soft-glass-light", darkProfile = "@soft-glass-dark";
  bool operator==(const HyprlandProfileSwitchingConfig&) const = default;
};

enum class HyprlandCursorMode : std::uint8_t { Tilt, Rotate, Stretch, None };
constexpr EnumOption<HyprlandCursorMode> kHyprlandCursorModes[] = {
    {HyprlandCursorMode::Tilt, "tilt", "settings.hyprland-input.tilt"},
    {HyprlandCursorMode::Rotate, "rotate", "settings.hyprland-input.rotate"},
    {HyprlandCursorMode::Stretch, "stretch", "settings.hyprland-input.stretch"},
    {HyprlandCursorMode::None, "none", "settings.hyprland-input.none"},
};

enum class HyprlandPointerAcceleration : std::uint8_t { Default, Adaptive, Flat };
enum class HyprlandDragLock : std::uint8_t { Disabled, Timeout, Sticky };
enum class HyprlandTapMap : std::uint8_t { Default, Lrm, Lmr };
enum class HyprlandDragFingers : std::uint8_t { Disabled, Three, Four };
constexpr EnumOption<HyprlandPointerAcceleration> kHyprlandPointerAcceleration[] = {
    {HyprlandPointerAcceleration::Default, "default", "settings.hyprland-input.HyprlandPointerAcceleration-default"},
    {HyprlandPointerAcceleration::Adaptive, "adaptive", "settings.hyprland-input.HyprlandPointerAcceleration-adaptive"},
    {HyprlandPointerAcceleration::Flat, "flat", "settings.hyprland-input.HyprlandPointerAcceleration-flat"},
};
constexpr EnumOption<HyprlandDragLock> kHyprlandDragLock[] = {
    {HyprlandDragLock::Disabled, "off", "settings.hyprland-input.HyprlandDragLock-off"},
    {HyprlandDragLock::Timeout, "timeout", "settings.hyprland-input.HyprlandDragLock-timeout"},
    {HyprlandDragLock::Sticky, "sticky", "settings.hyprland-input.HyprlandDragLock-sticky"},
};
constexpr EnumOption<HyprlandTapMap> kHyprlandTapMap[] = {
    {HyprlandTapMap::Default, "default", "settings.hyprland-input.HyprlandTapMap-default"},
    {HyprlandTapMap::Lrm, "lrm", "settings.hyprland-input.HyprlandTapMap-lrm"},
    {HyprlandTapMap::Lmr, "lmr", "settings.hyprland-input.HyprlandTapMap-lmr"},
};
constexpr EnumOption<HyprlandDragFingers> kHyprlandDragFingers[] = {
    {HyprlandDragFingers::Disabled, "off", "settings.hyprland-input.HyprlandDragFingers-off"},
    {HyprlandDragFingers::Three, "three", "settings.hyprland-input.HyprlandDragFingers-three"},
    {HyprlandDragFingers::Four, "four", "settings.hyprland-input.HyprlandDragFingers-four"},
};
enum class HyprlandLayoutSwitch : std::uint8_t { None, AltShift, CtrlShift, SuperSpace, AltSpace, BothShifts };
enum class HyprlandCapsLock : std::uint8_t { Normal, Escape, Control, SwapEscape, SwapControl, Disabled };
constexpr EnumOption<HyprlandLayoutSwitch> kHyprlandLayoutSwitch[] = {
    {HyprlandLayoutSwitch::None, "none", "settings.hyprland-input.LayoutSwitch-none"},
    {HyprlandLayoutSwitch::AltShift, "alt-shift", "settings.hyprland-input.LayoutSwitch-alt-shift"},
    {HyprlandLayoutSwitch::CtrlShift, "ctrl-shift", "settings.hyprland-input.LayoutSwitch-ctrl-shift"},
    {HyprlandLayoutSwitch::SuperSpace, "super-space", "settings.hyprland-input.LayoutSwitch-super-space"},
    {HyprlandLayoutSwitch::AltSpace, "alt-space", "settings.hyprland-input.LayoutSwitch-alt-space"},
    {HyprlandLayoutSwitch::BothShifts, "both-shifts", "settings.hyprland-input.LayoutSwitch-both-shifts"},
};
constexpr EnumOption<HyprlandCapsLock> kHyprlandCapsLock[] = {
    {HyprlandCapsLock::Normal, "normal", "settings.hyprland-input.CapsLock-normal"},
    {HyprlandCapsLock::Escape, "escape", "settings.hyprland-input.CapsLock-escape"},
    {HyprlandCapsLock::Control, "control", "settings.hyprland-input.CapsLock-control"},
    {HyprlandCapsLock::SwapEscape, "swap-escape", "settings.hyprland-input.CapsLock-swap-escape"},
    {HyprlandCapsLock::SwapControl, "swap-control", "settings.hyprland-input.CapsLock-swap-control"},
    {HyprlandCapsLock::Disabled, "disabled", "settings.hyprland-input.CapsLock-disabled"},
};
struct HyprlandDisplayConfig {
  std::string output;
  bool managed = false;
  std::string mode; // Empty means retain the compositor's configured mode.
  int transform = -1;
  float scale = 0; // Zero retains the compositor scale.
  bool positionManaged = false;
  int x = 0, y = 0;
  int vrr = -2; // -2 retains the rule; -1 follows Hyprland's global VRR setting.
  std::string colorMode;
  int bitDepth = 0;
  float sdrBrightness = 0, sdrSaturation = 0; // Zero retains the configured value.
  bool operator==(const HyprlandDisplayConfig&) const = default;
};

enum class HyprlandGestureFingers : std::uint8_t { Three = 3, Four = 4 };
constexpr EnumOption<HyprlandGestureFingers> kHyprlandGestureFingers[] = {
    {HyprlandGestureFingers::Three, "three", "settings.hyprland-input.gesture-three"},
    {HyprlandGestureFingers::Four, "four", "settings.hyprland-input.gesture-four"},
};
enum class HyprlandGestureDirection : std::uint8_t { Up, Down };
constexpr EnumOption<HyprlandGestureDirection> kHyprlandGestureDirection[] = {
    {HyprlandGestureDirection::Up, "up", "settings.hyprland-input.gesture-up"},
    {HyprlandGestureDirection::Down, "down", "settings.hyprland-input.gesture-down"},
};
struct HyprlandInputConfig {
  bool gesturesManaged = false;
  bool workspaceGestureEnabled = true;
  HyprlandGestureFingers workspaceGestureFingers = HyprlandGestureFingers::Three;
  float workspaceGestureSensitivity = 1;
  int workspaceGestureDistance = 300;
  bool workspaceGestureInvert = true;
  bool overviewGestureEnabled = true;
  HyprlandGestureFingers overviewGestureFingers = HyprlandGestureFingers::Four;
  HyprlandGestureDirection overviewGestureDirection = HyprlandGestureDirection::Up;
  int overviewGestureDistance = 100;

  bool keyboardManaged = false;
  std::string keyboardLayout = "us", keyboardLayout2 = "none", keyboardLayout3 = "none", keyboardLayout4 = "none";
  HyprlandLayoutSwitch keyboardLayoutSwitch = HyprlandLayoutSwitch::None;
  HyprlandCapsLock keyboardCapsLock = HyprlandCapsLock::Normal;
  int keyboardRepeatRate = 25, keyboardRepeatDelay = 600;
  bool keyboardNumLock = false;

  bool mouseManaged = false;
  float pointerSensitivity = 0;
  HyprlandPointerAcceleration pointerAcceleration = HyprlandPointerAcceleration::Default;
  bool mouseLeftHanded = false;
  bool mouseNaturalScroll = false;
  float mouseScrollFactor = 1;
  bool touchpadManaged = false;
  bool touchpadNaturalScroll = false;
  float touchpadScrollFactor = 1;
  bool touchpadTapToClick = true;
  bool touchpadTapAndDrag = true;
  HyprlandDragLock touchpadDragLock = HyprlandDragLock::Disabled;
  HyprlandTapMap touchpadTapMap = HyprlandTapMap::Default;
  bool touchpadClickfinger = false;
  bool touchpadMiddleEmulation = false;
  bool touchpadDisableWhileTyping = true;
  HyprlandDragFingers touchpadDragFingers = HyprlandDragFingers::Disabled;

  bool cursorManaged = false;
  bool cursorEnabled = true;
  HyprlandCursorMode cursorMode = HyprlandCursorMode::Tilt;
  int tiltLimit = 5000;
  int tiltAngle = 60;
  int cursorWindow = 100;
  int stretchLimit = 3000;
  int rotateLength = 32;
  bool shakeEnabled = true;
  float shakeThreshold = 6;
  float shakeLimit = 6;
  int shakeTimeout = 2000;
  bool scrollManaged = false;
  bool scrollEnabled = true;
  float scrollDecay = .92F;
  float scrollMultiplier = 1.25F;
  float scrollCutoff = .5F;
  int scrollInterval = 16;
  bool scrollBrowser = true;
  bool scrollStopClick = true;
  bool scrollStopFocus = false;
  bool scrollStopTarget = true;
  std::string scrollExcluded = "";
  bool edgeManaged = false;
  bool edgeEnabled = true;
  bool edgeLeft = true;
  bool edgeRight = true;
  bool edgeTop = true;
  bool edgeBottom = true;
  int edgeDistance = 0;
  int edgeFocus = -1;
  bool edgeClick = true;
  bool edgeScroll = true;
  bool operator==(const HyprlandInputConfig&) const = default;
};

struct HyprlandWindowBehaviourConfig {
  bool focusManaged = false;
  int focusMode = 1;
  float focusThreshold = 0;
  bool mouseRefocus = true;
  bool resizeManaged = false;
  bool resizeOnBorder = false;
  int borderGrab = 15;
  bool borderCursor = true;
  bool snapManaged = false;
  bool snapEnabled = false;
  int snapWindowDistance = 10;
  int snapMonitorDistance = 10;
  bool snapBorderOverlap = false;
  bool snapRespectGaps = false;
  bool activationManaged = false;
  bool focusOnActivate = false;
  bool operator==(const HyprlandWindowBehaviourConfig&) const = default;
};

enum class HyprlandTilingLayout : std::uint8_t { Dwindle, Master, Scrolling, Monocle };
constexpr EnumOption<HyprlandTilingLayout> kHyprlandTilingLayouts[] = {
    {HyprlandTilingLayout::Dwindle, "dwindle", "settings.hyprland-tiling.kHyprlandTilingLayouts-dwindle"},
    {HyprlandTilingLayout::Master, "master", "settings.hyprland-tiling.kHyprlandTilingLayouts-master"},
    {HyprlandTilingLayout::Scrolling, "scrolling", "settings.hyprland-tiling.kHyprlandTilingLayouts-scrolling"},
    {HyprlandTilingLayout::Monocle, "monocle", "settings.hyprland-tiling.kHyprlandTilingLayouts-monocle"},
};

enum class HyprlandMasterOrientation : std::uint8_t { Left, Right, Top, Bottom, Center };
constexpr EnumOption<HyprlandMasterOrientation> kHyprlandMasterOrientations[] = {
    {HyprlandMasterOrientation::Left, "left", "settings.hyprland-tiling.kHyprlandMasterOrientations-left"},
    {HyprlandMasterOrientation::Right, "right", "settings.hyprland-tiling.kHyprlandMasterOrientations-right"},
    {HyprlandMasterOrientation::Top, "top", "settings.hyprland-tiling.kHyprlandMasterOrientations-top"},
    {HyprlandMasterOrientation::Bottom, "bottom", "settings.hyprland-tiling.kHyprlandMasterOrientations-bottom"},
    {HyprlandMasterOrientation::Center, "center", "settings.hyprland-tiling.kHyprlandMasterOrientations-center"},
};

enum class HyprlandMasterStatus : std::uint8_t { Master, Slave, Inherit };
constexpr EnumOption<HyprlandMasterStatus> kHyprlandMasterStatuses[] = {
    {HyprlandMasterStatus::Master, "master", "settings.hyprland-tiling.kHyprlandMasterStatuses-master"},
    {HyprlandMasterStatus::Slave, "slave", "settings.hyprland-tiling.kHyprlandMasterStatuses-slave"},
    {HyprlandMasterStatus::Inherit, "inherit", "settings.hyprland-tiling.kHyprlandMasterStatuses-inherit"},
};

enum class HyprlandMasterPosition : std::uint8_t { None, Before, After };
constexpr EnumOption<HyprlandMasterPosition> kHyprlandMasterPositions[] = {
    {HyprlandMasterPosition::None, "none", "settings.hyprland-tiling.kHyprlandMasterPositions-none"},
    {HyprlandMasterPosition::Before, "before", "settings.hyprland-tiling.kHyprlandMasterPositions-before"},
    {HyprlandMasterPosition::After, "after", "settings.hyprland-tiling.kHyprlandMasterPositions-after"},
};

struct HyprlandTilingConfig {
  bool layoutManaged = false;
  HyprlandTilingLayout layout = HyprlandTilingLayout::Dwindle;
  bool dwindleManaged = false;
  bool preserveSplit = false;
  bool smartSplit = false;
  int forceSplit = 0;
  bool useActiveForSplits = true;
  float defaultSplitRatio = 1;
  float splitWidthMultiplier = 1;
  int splitBias = 0;
  bool masterManaged = false;
  float masterFactor = .55F;
  HyprlandMasterOrientation masterOrientation = HyprlandMasterOrientation::Left;
  HyprlandMasterStatus newStatus = HyprlandMasterStatus::Slave;
  HyprlandMasterPosition newOnActive = HyprlandMasterPosition::None;
  bool newOnTop = false;
  bool specialManaged = false;
  bool closeSpecialOnEmpty = true;
  bool hideSpecialOnWorkspaceChange = false;
  bool specialFallthrough = false;
  int warpOnSpecial = 0;
  bool operator==(const HyprlandTilingConfig&) const = default;
};

struct HyprlandKeybindConfig {
  std::string name;
  bool enabled = false;
  std::string chord;
  std::string action = "workspace", target = "1";
  bool replaceExisting = false;
  bool repeating = false;
  bool operator==(const HyprlandKeybindConfig&) const = default;
};

struct HyprlandWorkspaceConfig {
  std::string workspace; // Concrete positive number or name:<name>, never a selector.
  bool enabled = false;
  std::string label, icon, monitor;
  bool persistent = false;
  bool operator==(const HyprlandWorkspaceConfig&) const = default;
};

struct ShellConfig {
  std::vector<HyprlandWorkspaceConfig> hyprlandWorkspaces;
  std::vector<HyprlandKeybindConfig> hyprlandKeybinds;
  HyprlandTilingConfig hyprlandTiling;
  HyprlandWindowBehaviourConfig hyprlandWindowBehaviour;
  HyprlandInputConfig hyprlandInput;
  std::vector<HyprlandDisplayConfig> hyprlandDisplays;
  HyprlandProfileSwitchingConfig hyprlandProfileSwitching;
  std::vector<HyprlandAppRule> hyprlandAppRules;
  std::vector<HyprlandPlacementRule> hyprlandPlacementRules;
  HyprlandAppearanceConfig hyprlandAppearance;
  std::unordered_map<std::string, std::string> hyprlandAppearanceProfiles;
  struct AnimationConfig {
    bool enabled = true;
    float speed = 1.0F;

    bool operator==(const AnimationConfig&) const = default;
  };

  struct ShadowConfig {
    ShadowDirection direction = ShadowDirection::Down;
    float alpha = 0.35F;

    bool operator==(const ShadowConfig&) const = default;
  };

  struct PanelConfig {
    PanelTransparencyMode transparencyMode = PanelTransparencyMode::Auto;
    bool borders = true;                   // outline on floating panel surfaces
    bool shadow = true;                    // cast the global [shell.shadow] from panel surfaces
    bool listItemBackground = false;       // filled rounded background behind launcher/clipboard list items
    std::string floatingLayer = "overlay"; // top | overlay; attached panels follow their bar
    PanelPlacement launcherPlacement = PanelPlacement::Floating;
    PanelPlacement clipboardPlacement = PanelPlacement::Floating;
    PanelPlacement controlCenterPlacement = PanelPlacement::Attached;
    PanelPlacement wallpaperPlacement = PanelPlacement::Attached;
    PanelPlacement sessionPlacement = PanelPlacement::Attached;
    PanelPlacement polkitPlacement = PanelPlacement::Floating;
    // Floating screen position per panel (one of kPanelPositions). "auto" = bar-relative.
    // Launcher/clipboard default to "center" (the historical center-screen behavior).
    std::string launcherPosition = "center";
    std::string clipboardPosition = "center";
    std::string controlCenterPosition = "auto";
    std::string wallpaperPosition = "auto";
    std::string sessionPosition = "auto";
    std::string polkitPosition = "center";
    std::int32_t floatingOffset = 8; // logical px gap between a floating/detached panel and the bar edge
    bool openNearClickControlCenter = false;
    bool openNearClickLauncher = false;
    bool openNearClickClipboard = false;
    bool openNearClickWallpaper = false;
    bool openNearClickSession = false;

    bool operator==(const PanelConfig&) const = default;
  };

  // Launcher behavior/appearance. Panel placement for the launcher surface stays
  // under [shell.panel] (launcher_placement/position/open_near_click_launcher),
  // parallel to every other surface.
  struct LauncherConfig {
    // Off by default: Raycast shows only the search field and the list. F6 or the setting brings them back.
    bool categories = false;
    bool showIcons = true;
    bool showAppOriginIndicator = true;
    bool compact = false;
    bool appGrid = false;
    bool showAppActions = false;
    bool sortByUsage = true;
    // Desktop entry IDs shown first in the launcher when it opens without a query.
    std::vector<std::string> pinned;
    /// When true, refresh currency exchange rates from libqalculate's online sources.
    bool fetchExchangeRates = true;
    std::string providerPrefix = "/";
    /// Paste shortcut after a copy-style launcher activation (calculator, emoji, …).
    ClipboardAutoPasteMode autoPaste = ClipboardAutoPasteMode::Auto;

    struct DmenuConfig {
      std::vector<DmenuEntryConfig> entries;

      bool operator==(const DmenuConfig&) const = default;
    } dmenu;

    struct PanelsConfig {
      // Panel ids the panel provider never lists. Setting this in config.toml
      // replaces the default outright, same as every other list config here.
      std::vector<std::string> ignored{"polkit", "setup-wizard", "test", "launcher"};

      bool operator==(const PanelsConfig&) const = default;
    } panels;

    std::vector<LauncherProviderConfig> providers;

    // Search URL for the "Search the web" fallback; {query} is replaced by the typed text.
    std::string webSearchUrl = "https://duckduckgo.com/?q={query}";
    // Raycast's fallback commands: provider names ("files", "emoji", …) and quicklink ids ("github")
    // offered under "Use “query” with…" after the results, beside the web search.
    std::vector<std::string> fallbacks{"files"};
    // Markdown file quick notes are appended to; empty means ~/Documents/Notes.md (or ~/Notes.md).
    std::string notesFile;
    // Quicklinks and snippets from config. With no quicklinks configured a small built-in set is used.
    std::vector<LauncherQuicklinkConfig> quicklinks;
    std::vector<LauncherSnippetConfig> snippets;
    // Alias -> target, where target is a desktop entry id ("firefox") or "<provider>:<result id>".
    // Aliases set from the launcher's actions menu are kept in its state file instead.
    std::unordered_map<std::string, std::string> aliases;
    // Folders scanned for script commands. Empty means $XDG_CONFIG_HOME/noctalia/scripts.
    std::vector<std::string> scriptDirectories;

    bool operator==(const LauncherConfig&) const = default;
  };

  struct KeyboardLayoutConfig {
    std::unordered_map<std::string, std::string> customLabels;

    bool operator==(const KeyboardLayoutConfig&) const = default;
  };

  struct ScreenCornersConfig {
    bool enabled = false;
    std::int32_t size = 32;

    bool operator==(const ScreenCornersConfig&) const = default;
  };

  struct MprisConfig {
    std::vector<std::string> blacklist;

    bool operator==(const MprisConfig&) const = default;
  };

  struct ScreenshotConfig {
    bool saveToFile = true;
    bool copyToClipboard = true;
    bool freezeScreen = true;
    bool confirmRegion = false;
    bool rememberLastRegion = false;
    bool showCursor = false;
    bool annotate = false;
    bool skipAnnotateOnCopySave = false;
    bool closeOnCopy = true;
    bool closeOnSave = true;
    bool pipeToCommand = false;
    std::string pipeCommand;
    std::string directory;       // empty = XDG Pictures directory
    std::string filenamePattern; // empty = screenshot_%Y%m%d_%H%M%S

    bool operator==(const ScreenshotConfig&) const = default;
  };

  struct PrivacyConfig {
    std::string micFilterRegex;
    std::string camFilterRegex;
    std::string screenFilterRegex;

    bool operator==(const PrivacyConfig&) const = default;
  };

  enum class WindowSwitcherStyle : std::uint8_t {
    Carousel = 0,
    Compact = 1,
  };

  static constexpr EnumOption<WindowSwitcherStyle> kWindowSwitcherStyles[] = {
      {WindowSwitcherStyle::Carousel, "carousel", "settings.options.shell.window-switcher-style.carousel"},
      {WindowSwitcherStyle::Compact, "compact", "settings.options.shell.window-switcher-style.compact"},
  };

  struct WindowSwitcherConfig {
    WindowSwitcherStyle style = WindowSwitcherStyle::Carousel;
    bool mru = false;
    bool showCaption = true;
    bool showCount = true;
    bool showAppIcon = true;
    bool showAllOutputs = true;
    bool currentWorkspaceOnly = false;

    bool operator==(const WindowSwitcherConfig&) const = default;
  };

  float cornerRadiusScale = 1.0F;
  bool buttonBorders = true;
  bool inputBorders = true;
  bool popupBorders = true;
  bool popupShadows = true;
  bool cardBorders = true;
  std::string fontFamily = font_defaults::kFamily;
  std::string lang; // empty = auto-detect from $LC_ALL/$LC_MESSAGES/$LANG
  std::string timeFormat = "{:%H:%M}";
  std::string dateFormat = "%A %-d %B";
  bool offlineMode = false;
  /// Bar name panels attach to when opened without a source bar (IPC, shortcuts, dock).
  /// Empty keeps per-source resolution (widget click bar, else first enabled bar).
  std::string panelAnchorBar;
  /// Resolve and show the connection's external (WAN) IP in the Control Center network tab.
  bool externalIpEnabled = false;
  bool telemetryEnabled = false;
  bool setupWizardEnabled = true;
  bool niriOverviewTypeToLaunchEnabled = false;
  bool umbrielOverviewTypeToLaunchEnabled = false;
  bool polkitAgent = false;
  PasswordMaskStyle passwordMaskStyle = PasswordMaskStyle::CircleFilled;
  AnimationConfig animation;
  std::string avatarPath;
  bool settingsShowAdvanced = true;
  bool settingsExpandAllGroups = false;
  bool settingsWindowTranslucent = true; // follows panel transparency_mode; solid without compositor blur
  bool showLocation = true;
  bool appIconColorize = false;
  std::optional<ColorSpec> appIconColor;
  bool launchAppsAsSystemdServices = false;
  std::string launchAppsCustomCommand;
  std::string preferredTerminal; // Installed terminal desktop ID; empty uses system discovery.
  /// When false, disables Wayland clipboard integration (history panel, data-control binding, Input paste/copy hooks).
  bool clipboardEnabled = true;
  /// When true, the shell takes over the selection once the application that copied it exits, so the last copied item
  /// stays pasteable. Independent of history retention: this keeps the live clipboard, not the stored history.
  bool clipboardKeepFromClosedApps = true;
  /// Maximum unpinned clipboard history entries retained (pinned entries are exempt).
  int clipboardHistoryMaxEntries = static_cast<int>(noctalia::config::kClipboardHistoryDefaultEntries);
  /// When true, clearing clipboard history or deleting unpinned entries from the panel asks for confirmation first.
  bool clipboardConfirmClearHistory = true;
  /// Disables per-app tracking and Control Center usage UI.
  bool screenTimeEnabled = false;
  bool sharedGlContext = true;
  bool disableMipmaps = false;
  ClipboardAutoPasteMode clipboardAutoPaste = ClipboardAutoPasteMode::Auto;
  std::string clipboardImageActionCommand;
  ShadowConfig shadow;
  PanelConfig panel;
  LauncherConfig launcher;
  WindowSwitcherConfig windowSwitcher;
  KeyboardLayoutConfig keyboardLayout;
  ScreenCornersConfig screenCorners;
  MprisConfig mpris;
  ScreenshotConfig screenshot;
  PrivacyConfig privacy;
  ShellSessionConfig session;
  ShellGreeterSyncConfig greeterSync;

  bool operator==(const ShellConfig&) const = default;
};

struct WeatherConfig {
  bool enabled = true;
  bool effects = true;
  std::int32_t refreshMinutes = 30;
  std::string unit = "metric";

  bool operator==(const WeatherConfig&) const = default;
};

struct StorageConfig {
  StorageKeySource keySource = StorageKeySource::SecretService;
  std::string keyFile;

  bool operator==(const StorageConfig&) const = default;
};

enum class CalendarCredentialSource : std::uint8_t {
  SecretService = 0,
  File = 1,
};

struct CalendarConfig {
  // A single connected account. Google refresh tokens and Secret Service-backed CalDAV passwords
  // are not stored here. id must be [a-z0-9_] because it identifies durable credential records.
  struct Account {
    std::string id;
    std::string type; // "google" | "caldav" | "ics" | "vdir"
    std::string displayName;
    std::string color;                  // optional "#rrggbb" override
    std::string provider;               // "icloud" | "custom" (caldav only)
    std::string serverUrl;              // CalDAV discovery root (custom only; provider presets own theirs)
    std::string username;               // CalDAV login (caldav only)
    std::vector<std::string> calendars; // discovered collection ids; empty = all
    CalendarCredentialSource credentialSource = CalendarCredentialSource::SecretService; // CalDAV only
    std::string passwordFile; // required for file-backed CalDAV credentials
    std::string path;         // directory path for vdir/local accounts

    bool operator==(const Account&) const = default;
  };

  // Event reminder notifications. Gated by CalendarConfig::enabled.
  struct Reminders {
    bool enabled = true;
    // Honor per-event reminders (VALARM triggers, Google reminder overrides). When false, every
    // event uses defaultLeadMinutes instead.
    bool useEventReminders = true;
    // Fallback lead for events that carry no reminder of their own. 0 = notify at event start.
    std::int32_t defaultLeadMinutes = 10;
    // "HH:MM" local time for the once-a-day all-day event digest; empty disables it.
    std::string allDayDigestTime = "09:00";

    bool operator==(const Reminders&) const = default;
  };

  bool enabled = false;
  std::int32_t refreshMinutes = 15;
  std::string eventDateFormat = "%A %e %B";
  std::string eventTimeFormat = "%H:%M";
  Reminders reminders;
  std::vector<Account> accounts;

  bool operator==(const CalendarConfig&) const = default;
};

struct SystemConfig {
  struct MonitorConfig {
    // A poll value of 0 disables that metric entirely (no sampling, no wakeups);
    // any non-zero value is clamped to [kMinPollSeconds, kMaxPollSeconds].
    static constexpr float kDisabledPollSeconds = 0.0F;
    static constexpr float kMinPollSeconds = 1.0F;
    static constexpr float kMaxPollSeconds = 120.0F;

    bool enabled = true;
    std::string cpuTempSensorPath;
    float cpuPollSeconds = 2.0F;
    // GPU probes only run while something displays a GPU stat (SystemMonitorService retain counts),
    // so an idle machine never wakes a discrete GPU. Set to 0 to stop probing it entirely.
    float gpuPollSeconds = 5.0F;
    float memoryPollSeconds = 2.0F;
    float networkPollSeconds = 3.0F;
    float diskPollSeconds = 10.0F;
    double cpuUsageActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuUsage).activityDefault;
    double cpuUsageCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuUsage).criticalDefault;
    double cpuTempActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuTemp).activityDefault;
    double cpuTempCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuTemp).criticalDefault;
    double cpuFreqActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuFreq).activityDefault;
    double cpuFreqCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuFreq).criticalDefault;
    double gpuTempActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuTemp).activityDefault;
    double gpuTempCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuTemp).criticalDefault;
    double gpuUsageActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuUsage).activityDefault;
    double gpuUsageCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuUsage).criticalDefault;
    double gpuVramActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuVram).activityDefault;
    double gpuVramCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuVram).criticalDefault;
    double ramPctActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::RamPct).activityDefault;
    double ramPctCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::RamPct).criticalDefault;
    double swapPctActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::SwapPct).activityDefault;
    double swapPctCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::SwapPct).criticalDefault;
    double diskUsedPctActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskUsedPct).activityDefault;
    double diskUsedPctCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskUsedPct).criticalDefault;
    double diskUsedActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskUsed).activityDefault;
    double diskUsedCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskUsed).criticalDefault;
    double diskFreePctActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskFreePct).activityDefault;
    double diskFreePctCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskFreePct).criticalDefault;
    double diskFreeActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskFree).activityDefault;
    double diskFreeCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskFree).criticalDefault;
    double netRxActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetRx).activityDefault;
    double netRxCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetRx).criticalDefault;
    double netTxActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetTx).activityDefault;
    double netTxCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetTx).criticalDefault;

    bool operator==(const MonitorConfig&) const = default;
  };

  MonitorConfig monitor;

  bool operator==(const SystemConfig&) const = default;
};

struct AudioConfig {
  bool enableOverdrive = false;
  bool enableSounds = true;
  float soundVolume = 0.5F;
  std::string soundTheme = "freedesktop";

  bool operator==(const AudioConfig&) const = default;
};

// Normalized volume ceiling: overdrive raises it to 150%.
[[nodiscard]] inline float maxAudioVolume(const AudioConfig& audio) noexcept {
  return audio.enableOverdrive ? 1.5F : 1.0F;
}

enum class BrightnessBackendPreference : std::uint8_t {
  Auto = 0,
  None = 1,
  Backlight = 2,
  Ddcutil = 3,
};

constexpr EnumOption<BrightnessBackendPreference> kBrightnessBackendPreferences[] = {
    {BrightnessBackendPreference::Auto, "auto", ""},
    {BrightnessBackendPreference::None, "none", ""},
    {BrightnessBackendPreference::Backlight, "backlight", ""},
    {BrightnessBackendPreference::Ddcutil, "ddcutil", ""},
};

struct BrightnessMonitorOverride {
  std::string match;
  std::optional<BrightnessBackendPreference> backend;
  std::optional<std::string> backlightDevice; // sysfs device name or path, e.g. "intel_backlight"
  std::optional<std::int32_t> ddcBus;         // DDC bus number, e.g. 6 for i2c-6

  bool operator==(const BrightnessMonitorOverride&) const = default;
};

struct BrightnessConfig {
  bool enableDdcutil = false;
  bool syncBrightnessOfAllMonitors = false;
  std::vector<std::string> ddcutilIgnoreMmids;
  std::vector<BrightnessMonitorOverride> monitorOverrides;
  float minimumBrightness = 0.0F;

  bool operator==(const BrightnessConfig&) const = default;
};

struct BatteryDeviceWarningThreshold {
  std::string selector;
  // 0 disables the low-battery warning notification and widget warning state for this device.
  std::int32_t warningThreshold = 10;

  bool operator==(const BatteryDeviceWarningThreshold&) const = default;
};

struct BatteryConfig {
  // 0 disables the low-battery warning notification and widget warning state by default.
  std::int32_t warningThreshold = 10;
  std::vector<BatteryDeviceWarningThreshold> deviceThresholds;

  bool operator==(const BatteryConfig&) const = default;
};

struct KeybindsConfig {
  std::vector<KeyChord> validate;
  std::vector<KeyChord> cancel;
  std::vector<KeyChord> left;
  std::vector<KeyChord> right;
  std::vector<KeyChord> up;
  std::vector<KeyChord> down;
  std::vector<KeyChord> tabNext;
  std::vector<KeyChord> tabPrevious;
  std::vector<KeyChord> deleteEntry;
  std::vector<KeyChord> copy;
  std::vector<KeyChord> save;

  bool operator==(const KeybindsConfig&) const = default;
};

struct NightLightConfig {
  // Day temperature must be higher than night temperature by at least this much.
  static constexpr std::int32_t kTemperatureMin = 1000;
  static constexpr std::int32_t kTemperatureMax = 10000;
  static constexpr std::int32_t kTemperatureGap = 100;

  bool enabled = false;
  bool force = false;
  std::int32_t dayTemperature = 6500;
  std::int32_t nightTemperature = 4000;

  bool operator==(const NightLightConfig&) const = default;
};

struct LocationConfig {
  // Single source of truth for "where am I". Resolution priority:
  //   auto_locate (IP) -> address (geocoded) -> manual latitude/longitude.
  // When customSchedule is true, explicit sunset/sunrise times override coordinates.
  // Consumed by the weather service, night light, and theme auto mode.
  bool autoLocate = false;     // resolve coordinates from IP geolocation
  std::string address;         // geocoded when auto_locate is off and this is non-empty
  bool customSchedule = false; // when true, use sunset/sunrise times instead of coordinates
  std::string sunset;          // HH:MM night start, used only when customSchedule is true
  std::string sunrise;         // HH:MM day start, used only when customSchedule is true
  std::optional<double> latitude;
  std::optional<double> longitude;

  bool operator==(const LocationConfig&) const = default;
};

enum class HookKind : std::uint8_t {
  Started = 0,
  WallpaperChanged,
  ColorsChanged,
  ThemeModeChanged,
  SessionLocked,
  SessionUnlocked,
  LoggingOut,
  Rebooting,
  ShuttingDown,
  WifiEnabled,
  WifiDisabled,
  BluetoothEnabled,
  BluetoothDisabled,
  BatteryCharging,
  BatteryDischarging,
  BatteryPlugged,
  BatteryPercentageChanged,
  PowerProfileChanged,
  Count
};

constexpr EnumOption<HookKind> kHookKinds[] = {
    {HookKind::Started, "started", ""},
    {HookKind::WallpaperChanged, "wallpaper_changed", ""},
    {HookKind::ColorsChanged, "colors_changed", ""},
    {HookKind::ThemeModeChanged, "theme_mode_changed", ""},
    {HookKind::SessionLocked, "session_locked", ""},
    {HookKind::SessionUnlocked, "session_unlocked", ""},
    {HookKind::LoggingOut, "logging_out", ""},
    {HookKind::Rebooting, "rebooting", ""},
    {HookKind::ShuttingDown, "shutting_down", ""},
    {HookKind::WifiEnabled, "wifi_enabled", ""},
    {HookKind::WifiDisabled, "wifi_disabled", ""},
    {HookKind::BluetoothEnabled, "bluetooth_enabled", ""},
    {HookKind::BluetoothDisabled, "bluetooth_disabled", ""},
    {HookKind::BatteryCharging, "battery_charging", ""},
    {HookKind::BatteryDischarging, "battery_discharging", ""},
    {HookKind::BatteryPlugged, "battery_plugged", ""},
    {HookKind::BatteryPercentageChanged, "battery_percentage_changed", ""},
    {HookKind::PowerProfileChanged, "power_profile_changed", ""},
};

static_assert(sizeof(kHookKinds) / sizeof(kHookKinds[0]) == static_cast<std::size_t>(HookKind::Count));

struct HooksConfig {
  std::array<std::vector<std::string>, static_cast<std::size_t>(HookKind::Count)> commands{};

  bool operator==(const HooksConfig&) const = default;
};

std::optional<HookKind> hookKindFromKey(std::string_view key);
std::string_view hookKindKey(HookKind kind);

enum class PaletteSource : std::uint8_t {
  Builtin = 0,
  Wallpaper = 1,
  Community = 2,
  Custom = 3,
};

constexpr EnumOption<PaletteSource> kPaletteSources[] = {
    {PaletteSource::Builtin, "builtin", "settings.options.theme.source.built-in"},
    {PaletteSource::Wallpaper, "wallpaper", "settings.options.theme.source.wallpaper"},
    {PaletteSource::Community, "community", "settings.options.theme.source.community"},
    {PaletteSource::Custom, "custom", "settings.options.theme.source.custom"},
};

enum class ThemeMode : std::uint8_t {
  Dark = 0,
  Light = 1,
  Auto = 2,
};

constexpr EnumOption<ThemeMode> kThemeModes[] = {
    {ThemeMode::Light, "light", "settings.options.theme.mode.light"},
    {ThemeMode::Dark, "dark", "settings.options.theme.mode.dark"},
    {ThemeMode::Auto, "auto", "common.states.auto"},
};

// Noctalia's own light/dark mode. `follow` tracks [theme].mode, which always drives apps
// (templates and the GTK color scheme); the other values pin the shell independently.
enum class ShellThemeMode : std::uint8_t {
  Follow = 0,
  Dark = 1,
  Light = 2,
  Auto = 3,
};

constexpr EnumOption<ShellThemeMode> kShellThemeModes[] = {
    {ShellThemeMode::Follow, "follow", "settings.options.theme.shell-mode.follow"},
    {ShellThemeMode::Light, "light", "settings.options.theme.mode.light"},
    {ShellThemeMode::Dark, "dark", "settings.options.theme.mode.dark"},
    {ShellThemeMode::Auto, "auto", "common.states.auto"},
};

struct WallpaperFavorite {
  std::string path;
  std::optional<ThemeMode> themeMode;
  std::optional<PaletteSource> paletteSource;
  std::string builtinPalette;
  std::string communityPalette;
  std::string customPalette;
  std::string wallpaperScheme;

  bool operator==(const WallpaperFavorite&) const = default;
};

enum class ControlCenterSidebarMode : std::uint8_t {
  Full = 0,
  Compact = 1,
  None = 2,
};

constexpr EnumOption<ControlCenterSidebarMode> kControlCenterSidebarModes[] = {
    {ControlCenterSidebarMode::Full, "full", "settings.options.control-center.sidebar.full"},
    {ControlCenterSidebarMode::Compact, "compact", "settings.options.control-center.sidebar.compact"},
    {ControlCenterSidebarMode::None, "none", "settings.options.control-center.sidebar.none"},
};

struct ThemeConfig {
  struct TemplateColorConfig {
    std::string name;
    std::string color;
    std::string color_dark = "";
    std::string color_light = "";
    bool blend = true;

    bool operator==(const TemplateColorConfig&) const = default;
  };

  struct TemplateInputPathModesConfig {
    std::string dark;
    std::string light;

    bool operator==(const TemplateInputPathModesConfig&) const = default;
  };

  struct TemplateCompareColorConfig {
    std::string name;
    std::string color;

    bool operator==(const TemplateCompareColorConfig&) const = default;
  };

  struct UserTemplateConfig {
    std::string id;
    bool enabled = true;
    std::string inputPath;
    std::optional<TemplateInputPathModesConfig> inputPathModes;
    std::vector<std::string> outputPaths;
    std::string outputPathDynamic;
    std::string compareTo;
    std::vector<TemplateCompareColorConfig> colorsToCompare;
    std::string preHook;
    std::string postHook;
    std::string postAction;
    int index = 0;
    // False runs post_hook inline, serialized against other hooks of the same run.
    bool hookAsync = true;

    bool operator==(const UserTemplateConfig&) const = default;
  };

  struct TemplatesConfig {
    bool enableBuiltinTemplates = true;
    std::vector<std::string> builtinIds;
    bool enableCommunityTemplates = true;
    std::vector<std::string> communityIds;
    std::vector<TemplateColorConfig> customColors;
    std::vector<UserTemplateConfig> userTemplates;

    bool operator==(const TemplatesConfig&) const = default;
  };

  PaletteSource source = PaletteSource::Builtin;
  std::string builtinPalette = "Noctalia";
  std::string communityPalette = "Oxocarbon";
  std::string customPalette;
  std::string wallpaperScheme = "m3-content";
  ThemeMode mode = ThemeMode::Dark;
  ShellThemeMode shellMode = ShellThemeMode::Follow;
  bool pureBlackDark = false;
  TemplatesConfig templates;

  bool operator==(const ThemeConfig&) const = default;
};

// The theme mode Noctalia's own surfaces run in, still expressed as a ThemeMode so `auto`
// keeps resolving against the day/night schedule.
[[nodiscard]] constexpr ThemeMode shellThemeMode(const ThemeConfig& theme) noexcept {
  switch (theme.shellMode) {
  case ShellThemeMode::Dark:
    return ThemeMode::Dark;
  case ShellThemeMode::Light:
    return ThemeMode::Light;
  case ShellThemeMode::Auto:
    return ThemeMode::Auto;
  case ShellThemeMode::Follow:
    break;
  }
  return theme.mode;
}

struct ControlCenterConfig {
  static constexpr std::int32_t kDefaultWidth = 700;

  struct HomeTabConfig {
    std::vector<std::string> cards = {"profile", "media", "clock", "shortcuts"};
    bool stacked = false;
    bool operator==(const HomeTabConfig&) const = default;
  };

  struct CalendarTabConfig {
    bool showEventsCard = true;
    bool showWeekNumbers = false;
    bool operator==(const CalendarTabConfig&) const = default;
  };

  std::vector<ShortcutConfig> shortcuts;
  std::vector<std::string> hiddenTabs; // tab keys (see kTabs) the user has hidden; empty = all available shown
  ControlCenterSidebarMode sidebarMode = ControlCenterSidebarMode::Compact;
  ControlCenterSidebarMode sidebarSectionMode = ControlCenterSidebarMode::Compact;
  std::int32_t width = kDefaultWidth; // full-sidebar logical width; compact/none modes scale down from this
  bool showShortcutLabels = true;
  bool showSessionButton = true;
  HomeTabConfig homeTab;
  CalendarTabConfig calendarTab;
  bool operator==(const ControlCenterConfig&) const = default;
};

// A plugin source: where plugin code comes from. `Git` is a repo URL the host
// caches, updates, and exports plugin runtime files from; `Path` is an
// immutable local directory (e.g. a Nix store path) the host treats read-only
// (update/auto-update/remove are no-ops).
enum class PluginSourceKind : std::uint8_t {
  Git = 0,
  Path = 1,
};

constexpr EnumOption<PluginSourceKind> kPluginSourceKinds[] = {
    {PluginSourceKind::Git, "git", "settings.options.plugins.source.git"},
    {PluginSourceKind::Path, "path", "settings.options.plugins.source.path"},
};

// Background auto-update scope for git plugin sources.
enum class PluginAutoUpdateMode : std::uint8_t {
  None = 0,     // never auto-update
  Official = 1, // only the built-in "official" source
  All = 2,      // every enabled git source
};

constexpr EnumOption<PluginAutoUpdateMode> kPluginAutoUpdateModes[] = {
    {PluginAutoUpdateMode::All, "all", "settings.options.plugins.auto-update.all"},
    {PluginAutoUpdateMode::Official, "official", "settings.options.plugins.auto-update.official"},
    {PluginAutoUpdateMode::None, "none", "settings.options.plugins.auto-update.none"},
};

struct PluginSourceConfig {
  PluginSourceKind kind = PluginSourceKind::Git;
  std::string name;     // stable handle (also the clone subdir for git sources)
  std::string location; // git URL or local path
  bool enabled = true;  // disabled sources are not scanned; their clone is kept
  bool operator==(const PluginSourceConfig&) const = default;
};

// Distribution config: where plugins come from and which are turned on. User
// intent → config (declarative-friendly); clones live under the state dir.
struct PluginsConfig {
  std::vector<PluginSourceConfig> sources;
  std::vector<std::string> enabled; // active plugin ids ("author/plugin"); opt-in for every source
  PluginAutoUpdateMode autoUpdate = PluginAutoUpdateMode::All; // background auto-update scope (startup + every 6h)
  // Plugin-level setting overrides, keyed by plugin id then setting key. Seeded
  // into every entry runtime of the plugin (widget/shortcut/service). Open-ended
  // (validated against the manifest schema), so compared via configEqual rather
  // than the defaulted operator== (which lacks int/double coercion).
  std::unordered_map<std::string, std::unordered_map<std::string, WidgetSettingValue>> pluginSettings;
  bool operator==(const PluginsConfig&) const = default;
};

// Default sources seeded when [plugins] declares no [[plugins.source]]: the
// official + community plugin repos.
[[nodiscard]] std::vector<PluginSourceConfig> defaultPluginSources();
[[nodiscard]] bool isDefaultPluginSourceName(std::string_view name);
// Whether the background auto-update mode covers `source` (kind, enabled state, and
// official identity for PluginAutoUpdateMode::Official). The official source matches
// by name AND location, so a user-added source that reuses the name is not the
// official source. Pure, so the auto-update tick and its tests share one decision.
[[nodiscard]] bool sourceInAutoUpdateScope(const PluginSourceConfig& source, PluginAutoUpdateMode mode);
// Source names are stable user-facing handles and git source storage directory names.
// Keep them flat so they can never escape the plugin source cache.
[[nodiscard]] bool isValidPluginSourceName(std::string_view name);

struct AccessibilityConfig {
  float uiScale = 1.0F;
  bool highContrast = false;
  bool operator==(const AccessibilityConfig&) const = default;
};

struct HotCornersConfig {
  bool enabled = false;
  // Hold time in the corner before the action runs. 0 = trigger immediately on enter.
  std::int32_t delayMs = 0;

  struct Corner {
    std::string action = "none";
    std::string command;
    bool operator==(const Corner&) const = default;
  };

  Corner topLeft;
  Corner topRight;
  Corner bottomLeft;
  Corner bottomRight;

  bool operator==(const HotCornersConfig&) const = default;
};

struct Config {
  IslandConfig island;
  std::vector<BarConfig> bars;
  std::unordered_map<std::string, WidgetConfig> widgets;
  WallpaperConfig wallpaper;
  BackdropConfig backdrop;
  LockscreenConfig lockscreen;
  LockscreenWidgetsConfig lockscreenWidgets;
  DockConfig dock;
  DesktopWidgetsConfig desktopWidgets;
  HotCornersConfig hotCorners;
  StorageConfig storage;
  ShellConfig shell;
  OsdConfig osd;
  NotificationConfig notification;
  WeatherConfig weather;
  CalendarConfig calendar;
  SystemConfig system;
  AudioConfig audio;
  BrightnessConfig brightness;
  BatteryConfig battery;
  KeybindsConfig keybinds;
  NightLightConfig nightlight;
  LocationConfig location;
  IdleConfig idle;
  HooksConfig hooks;
  ThemeConfig theme;
  ControlCenterConfig controlCenter;
  PluginsConfig plugins;
  AccessibilityConfig accessibility;
};
// Which top-level config sections changed across a reload. Default-constructed
// to all-true (conservative: "assume everything changed") so any path that does
// not compute a precise diff still fans the reload out to every subscriber.
struct ConfigChangeSet {
  bool bars = true;
  bool widgets = true;
  bool desktopWidgets = true;
  bool lockscreenWidgets = true;
  bool wallpaper = true;
  bool backdrop = true;
  bool lockscreen = true;
  bool dock = true;
  bool shell = true;
  bool osd = true;
  bool island = true;
  bool notification = true;
  bool weather = true;
  bool calendar = true;
  bool system = true;
  bool audio = true;
  bool brightness = true;
  bool battery = true;
  bool keybinds = true;
  bool nightlight = true;
  bool location = true;
  bool idle = true;
  bool hooks = true;
  bool theme = true;
  bool controlCenter = true;
  bool plugins = true;
  bool hotCorners = true;
  bool storage = true;
  bool accessibility = true;

  [[nodiscard]] bool any() const noexcept {
    return bars
        || widgets
        || desktopWidgets
        || lockscreenWidgets
        || wallpaper
        || backdrop
        || lockscreen
        || dock
        || shell
        || osd
        || island
        || notification
        || weather
        || calendar
        || system
        || audio
        || brightness
        || battery
        || keybinds
        || nightlight
        || location
        || idle
        || hooks
        || theme
        || controlCenter
        || plugins
        || hotCorners
        || storage
        || accessibility;
  }
};

// Per-section diff using the same comparison semantics as configEqual()
// (bars/widgets/desktop widgets get their specialized comparators). Implemented
// in config_overrides.cpp alongside those comparators.
[[nodiscard]] ConfigChangeSet computeConfigChangeSet(const Config& prev, const Config& next);
