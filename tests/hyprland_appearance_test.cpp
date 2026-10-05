#include "compositors/hyprland/hyprland_appearance.h"
#include "compositors/hyprland/hyprland_displays.h"
#include "compositors/hyprland/hyprland_keybinds.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/input/key_modifiers.h"
#include "shell/bar/widgets/workspace_preferences.h"
#include "shell/bar/widgets/workspaces_widget_definition.h"
#include "shell/settings/display_layout.h"
#include "shell/settings/hyprland_editor.h"
#include "shell/settings/settings_registry.h"
#include "system/keyboard_layout_catalog.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <sstream>

int main() {
  using namespace noctalia::config::schema;
  using compositors::hyprland::placementRuleCommands;
  using compositors::hyprland::placementRuleProblem;
  using namespace compositors::hyprland;
  bool superRejected = false;
  try {
    (void)parseKeyChordSpec("Super+f");
  } catch (...) {
    superRejected = true;
  }
  assert(superRejected); // Shell-only shortcuts retain their existing policy.
  const auto superChord = compositorChord("ctrl + super + F");
  assert(superChord && superChord->modifiers == (KeyMod::Ctrl | KeyMod::Super));
  assert(compositorChordString(*superChord) == "SUPER+CTRL+f");
  assert(compositorChordString(*compositorChord("Super+Shift+ISO_Left_Tab")) == "SUPER+SHIFT+Tab");
  assert(keyChordToString(*superChord).find("Super") != std::string::npos);
  assert(keyChordDisplayLabel(*superChord).find("Super") != std::string::npos);
  for (const auto* bad : {"a", "Shift+a", "Super", "SUPER+unknown-key", "SUPER++f"})
    assert(!compositorChord(bad));
  HyprlandKeybindConfig shortcut;
  shortcut.name = "test";
  shortcut.chord = "SUPER+F";
  shortcut.enabled = true;
  shortcut.target = "8";
  nlohmann::json liveBinds = nlohmann::json::array({{{"key", "f"}, {"modmask", 64}, {"description", "Lua binding"}}});
  assert(keybindProblem(shortcut, {shortcut}, liveBinds) == "conflict");
  assert(keybindCommands({shortcut}, liveBinds).find("hl.bind(") == std::string::npos);
  shortcut.replaceExisting = true;
  assert(keybindProblem(shortcut, {shortcut}, liveBinds).empty());
  assert(keybindCommands({shortcut}, liveBinds).find(":remove()") != std::string::npos);
  assert(keybindCommands({shortcut}, liveBinds).find("workspace=\"8\"") != std::string::npos);
  liveBinds[0]["submap"] = "resize";
  assert(keybindProblem(shortcut, {shortcut}, liveBinds) == "submap-conflict");
  liveBinds[0]["submap"] = "";
  liveBinds[0]["keycode"] = 41;
  assert(keybindProblem(shortcut, {shortcut}, liveBinds) == "physical-conflict");
  liveBinds[0]["keycode"] = 0;
  liveBinds[0]["key"] = "";
  assert(keybindProblem(shortcut, {shortcut}, liveBinds) == "physical-conflict");
  liveBinds[0]["key"] = "f";
  liveBinds[0]["description"] = "Noctalia shortcut:test";
  assert(keybindProblem(shortcut, {shortcut}, liveBinds).empty());
  auto duplicate = shortcut;
  duplicate.name = "duplicate";
  duplicate.chord = "super+f";
  assert(keybindProblem(shortcut, {shortcut, duplicate}, liveBinds) == "duplicate");
  duplicate.enabled = false;
  assert(keybindProblem(shortcut, {shortcut, duplicate}, liveBinds).empty());
  assert(keybindProblem(shortcut, {shortcut}, nlohmann::json{}) == "unavailable");
  shortcut.action = "exec";
  shortcut.target = "app \"quote\"; touch /tmp/example";
  assert(keybindAction(shortcut).starts_with("hl.dsp.exec_cmd("));
  assert(keybindAction(shortcut).find("\\\"quote\\\"") != std::string::npos);
  for (const auto* action : {"fullscreen", "floating", "close", "overview"}) {
    shortcut.action = action;
    assert(!keybindAction(shortcut).empty());
  }
  shortcut.action = "move_direction";
  shortcut.target = "left";
  assert(!keybindAction(shortcut).empty());
  shortcut.target = "invalid";
  assert(keybindAction(shortcut).empty());
  ShellConfig shortcutsShell, shortcutsRestored;
  shortcutsShell.hyprlandKeybinds = {shortcut};
  Diagnostics shortcutsDiagnostics;
  readInto(writeTable(shortcutsShell, shellSchema()), shortcutsRestored, shellSchema(), "shell", shortcutsDiagnostics);
  assert(shortcutsRestored.hyprlandKeybinds == shortcutsShell.hyprlandKeybinds);
  HyprlandWorkspaceConfig workspace;
  workspace.workspace = "3";
  workspace.label = "Writing";
  workspace.icon = "★";
  workspace.monitor = "DP-2";
  workspace.persistent = true;
  assert(compositors::hyprland::workspaceRuleCommands({workspace}).empty());
  workspace.enabled = true;
  const auto wsCommand = compositors::hyprland::workspaceRuleCommands({workspace});
  assert(wsCommand.find("workspace=\"3\",persistent=true,monitor=\"DP-2\"") != std::string::npos);
  assert(wsCommand.find("Writing") == std::string::npos); // Presentation names do not rename native IDs.
  assert(workspace_preferences::label(workspace) == "★ Writing");
  std::vector<HyprlandWorkspaceConfig> workspaceRules{workspace};
  assert(workspace_preferences::find(workspaceRules, Workspace{.id = "3", .name = "Native name"}));
  assert(!workspace_preferences::find(workspaceRules, Workspace{.id = "4"}));
  for (const auto* target :
       {"0", "01", "1001", "-1", "r[1-3]", "name:", "name:bad silent", "special:foo", "3; bad()"}) {
    workspace.workspace = target;
    assert(!workspace_preferences::validTarget(target));
    assert(compositors::hyprland::workspaceRuleCommands({workspace}).empty());
  }
  workspace.workspace = "name:work";
  workspaceRules = {workspace};
  assert(workspace_preferences::find(workspaceRules, Workspace{.id = "-1337", .name = "work"}));
  assert(workspace_preferences::find(workspaceRules, Workspace{.id = "name:work", .name = "Work"}));
  workspace.monitor = "quote\";bad()";
  assert(
      compositors::hyprland::workspaceRuleCommands({workspace}).find(R"lua(monitor="quote\";bad()")lua")
      != std::string::npos
  );
  ShellConfig workspaceShell, workspaceRestored;
  workspaceShell.hyprlandWorkspaces = {workspace};
  Diagnostics workspaceDiagnostics;
  readInto(writeTable(workspaceShell, shellSchema()), workspaceRestored, shellSchema(), "shell", workspaceDiagnostics);
  assert(workspaceRestored.hyprlandWorkspaces == workspaceShell.hyprlandWorkspaces);
  using workspace_preferences::step;
  assert(!step(0, {}, true, true));
  assert(!step(3, 2, true, false));
  assert(!step(3, 0, false, false));
  assert(step(3, 2, true, true) == 0);
  assert(step(3, 0, false, true) == 2);
  assert(step(3, 1, true, false) == 2);
  assert(step(3, {}, false, false) == 2);
  assert(!step(1, 0, true, true));
  WidgetConfig workspaceWidget;
  workspaceWidget.settings["scroll_wrap"] = true;
  assert(workspacesWidgetDefinition().resolve(&workspaceWidget, "test").scrollWrap);
  assert(!workspacesWidgetDefinition().resolve(nullptr, "test").scrollWrap);
  HyprlandPlacementRule placement;
  placement.name = "app-0001";
  placement.appClass = "place.app+[one]";
  assert(!placement.enabled && placementRuleCommands({placement}).empty());
  placement.enabled = true;
  assert(placementRuleCommands({placement}).empty());
  placement.mode = HyprlandPlacementMode::Floating;
  placement.workspace = HyprlandPlacementWorkspace::Number;
  placement.workspaceNumber = 7;
  placement.sizeManaged = true;
  placement.width = 600;
  placement.height = 400;
  placement.position = HyprlandPlacementPosition::Offset;
  placement.x = 120;
  placement.y = 140;
  placement.pin = HyprlandRuleSwitch::On;
  const auto placementCommand = placementRuleCommands({placement});
  assert(
      placementCommand.find("float=true,workspace=\"7 silent\",size={600,400},move={120,140},pin=true")
      != std::string::npos
  );
  assert(placementCommand.find(R"(class="^place\\.app\\+\\[one\\]$")") != std::string::npos);
  assert(placementCommand.find("__noctalia_app_placement_v1") != std::string::npos);
  ShellConfig placementShell, placementRestored;
  placementShell.hyprlandPlacementRules = {placement};
  Diagnostics placementDiagnostics;
  readInto(writeTable(placementShell, shellSchema()), placementRestored, shellSchema(), "shell", placementDiagnostics);
  assert(placementRestored.hyprlandPlacementRules == placementShell.hyprlandPlacementRules);
  auto tiledPlacement = placement;
  tiledPlacement.mode = HyprlandPlacementMode::Tiled;
  const auto tiledCommand = placementRuleCommands({tiledPlacement});
  assert(tiledCommand.find("tile=true") != std::string::npos);
  for (const auto key : {"size=", "move=", "pin="})
    assert(tiledCommand.find(key) == std::string::npos);
  auto namedPlacement = placement;
  namedPlacement.workspace = HyprlandPlacementWorkspace::Named;
  namedPlacement.workspaceName = "Work-2";
  namedPlacement.workspaceSilent = false;
  namedPlacement.position = HyprlandPlacementPosition::Center;
  assert(placementRuleCommands({namedPlacement}).find("workspace=\"name:Work-2\"") != std::string::npos);
  assert(placementRuleCommands({namedPlacement}).find("center=true") != std::string::npos);
  namedPlacement.workspace = HyprlandPlacementWorkspace::Special;
  assert(placementRuleCommands({namedPlacement}).find("workspace=\"special:Work-2\"") != std::string::npos);
  for (const auto bad : {"", "bad silent", "space name", "name:foo", "quote\""}) {
    namedPlacement.workspaceName = bad;
    assert(placementRuleProblem(namedPlacement) == "invalid-workspace");
    assert(placementRuleCommands({namedPlacement}).empty());
  }
  namedPlacement = placement;
  namedPlacement.appClass.clear();
  assert(placementRuleProblem(namedPlacement) == "invalid-app");
  readInto(
      toml::parse(
          "[hyprland_placement_rules.bounded]\napp_class=\"test\"\nworkspace_number=0\nwidth=-1\nheight=99999\nx=-"
          "10\ny=99999"
      ),
      placementRestored, shellSchema(), "shell", placementDiagnostics
  );
  const auto boundedPlacement =
      std::ranges::find_if(placementRestored.hyprlandPlacementRules, [](const auto& r) { return r.name == "bounded"; });
  assert(boundedPlacement != placementRestored.hyprlandPlacementRules.end());
  assert(boundedPlacement->workspaceNumber == 1 && boundedPlacement->width == 100 && boundedPlacement->height == 8192);
  assert(boundedPlacement->x == 0 && boundedPlacement->y == 16384);
  using compositors::hyprland::tilingCommands;
  HyprlandTilingConfig tiling;
  assert(tilingCommands(tiling).empty());
  tiling.layoutManaged = tiling.dwindleManaged = tiling.masterManaged = tiling.specialManaged = true;
  tiling.layout = HyprlandTilingLayout::Master;
  tiling.preserveSplit = tiling.smartSplit = true;
  tiling.forceSplit = 2;
  tiling.useActiveForSplits = false;
  tiling.defaultSplitRatio = 1.25F;
  tiling.splitWidthMultiplier = 1.5F;
  tiling.splitBias = 1;
  tiling.masterFactor = .65F;
  tiling.masterOrientation = HyprlandMasterOrientation::Right;
  tiling.newStatus = HyprlandMasterStatus::Inherit;
  tiling.newOnActive = HyprlandMasterPosition::After;
  tiling.newOnTop = true;
  tiling.closeSpecialOnEmpty = false;
  tiling.hideSpecialOnWorkspaceChange = tiling.specialFallthrough = true;
  tiling.warpOnSpecial = 2;
  ShellConfig tilingShell, tilingRestored;
  tilingShell.hyprlandTiling = tiling;
  Diagnostics tilingDiagnostics;
  readInto(writeTable(tilingShell, shellSchema()), tilingRestored, shellSchema(), "shell", tilingDiagnostics);
  assert(tilingRestored.hyprlandTiling == tiling);
  readInto(
      toml::parse(
          "[hyprland_tiling]\nforce_split=7\ndefault_split_ratio=-1.0\nsplit_width_multiplier=9.0\nmaster_factor=2."
          "0\nwarp_on_special=-1"
      ),
      tilingRestored, shellSchema(), "shell", tilingDiagnostics
  );
  assert(tilingRestored.hyprlandTiling.forceSplit == 2);
  assert(tilingRestored.hyprlandTiling.defaultSplitRatio == .1F);
  assert(tilingRestored.hyprlandTiling.splitWidthMultiplier == 3);
  assert(tilingRestored.hyprlandTiling.masterFactor == 1);
  assert(tilingRestored.hyprlandTiling.warpOnSpecial == 0);
  const auto tilingCommand = tilingCommands(tiling);
  assert(tilingCommand.find("general={layout=\"master\"}") != std::string::npos);
  assert(tilingCommand.find("force_split=2,use_active_for_splits=false,default_split_ratio=1.25") != std::string::npos);
  assert(
      tilingCommand.find("orientation=\"right\",new_status=\"inherit\",new_on_active=\"after\",new_on_top=true")
      != std::string::npos
  );
  assert(tilingCommand.find("close_special_on_empty=false") != std::string::npos);
  assert(tilingCommand.find("warp_on_toggle_special=2") != std::string::npos);
  auto specialOnly = tiling;
  specialOnly.layoutManaged = specialOnly.dwindleManaged = specialOnly.masterManaged = false;
  assert(tilingCommands(specialOnly).find("layout=") == std::string::npos);
  assert(tilingCommands(specialOnly).find("dwindle=") == std::string::npos);
  assert(tilingCommands(specialOnly).find("master=") == std::string::npos);
  using compositors::hyprland::windowBehaviourCommands;
  HyprlandWindowBehaviourConfig behaviour;
  assert(windowBehaviourCommands(behaviour).empty());
  behaviour.focusManaged = behaviour.resizeManaged = behaviour.snapManaged = behaviour.activationManaged = true;
  behaviour.focusMode = 2;
  behaviour.focusThreshold = 8.5F;
  behaviour.mouseRefocus = false;
  behaviour.resizeOnBorder = true;
  behaviour.borderGrab = 23;
  behaviour.borderCursor = false;
  behaviour.snapEnabled = true;
  behaviour.snapWindowDistance = 12;
  behaviour.snapMonitorDistance = 17;
  behaviour.snapBorderOverlap = behaviour.snapRespectGaps = behaviour.focusOnActivate = true;
  ShellConfig behaviourShell, behaviourRestored;
  behaviourShell.hyprlandWindowBehaviour = behaviour;
  Diagnostics behaviourDiagnostics;
  readInto(writeTable(behaviourShell, shellSchema()), behaviourRestored, shellSchema(), "shell", behaviourDiagnostics);
  assert(behaviourRestored.hyprlandWindowBehaviour == behaviour);
  readInto(
      toml::parse(
          "[hyprland_window_behaviour]\nfocus_mode=9\nfocus_threshold=-2.0\nborder_grab=101\nsnap_window_distance=-"
          "4\nsnap_monitor_distance=200"
      ),
      behaviourRestored, shellSchema(), "shell", behaviourDiagnostics
  );
  assert(behaviourRestored.hyprlandWindowBehaviour.focusMode == 3);
  assert(behaviourRestored.hyprlandWindowBehaviour.focusThreshold == 0);
  assert(behaviourRestored.hyprlandWindowBehaviour.borderGrab == 100);
  assert(behaviourRestored.hyprlandWindowBehaviour.snapWindowDistance == 0);
  assert(behaviourRestored.hyprlandWindowBehaviour.snapMonitorDistance == 100);
  const auto behaviourCommand = windowBehaviourCommands(behaviour);
  assert(behaviourCommand.find("follow_mouse=2,follow_mouse_threshold=8.5,mouse_refocus=false") != std::string::npos);
  assert(
      behaviourCommand.find("resize_on_border=true,extend_border_grab_area=23,hover_icon_on_border=false")
      != std::string::npos
  );
  assert(
      behaviourCommand.find("snap={enabled=true,window_gap=12,monitor_gap=17,border_overlap=true,respect_gaps=true}")
      != std::string::npos
  );
  assert(behaviourCommand.find("focus_on_activate=true") != std::string::npos);
  auto snapOnly = behaviour;
  snapOnly.focusManaged = snapOnly.resizeManaged = snapOnly.activationManaged = false;
  assert(windowBehaviourCommands(snapOnly).find("input=") == std::string::npos);
  assert(windowBehaviourCommands(snapOnly).find("resize_on_border") == std::string::npos);
  assert(windowBehaviourCommands(snapOnly).find("misc=") == std::string::npos);
  using compositors::hyprland::inputMotionCommands;
  using compositors::hyprland::inputMotionPreset;
  HyprlandInputConfig input;
  assert(inputMotionCommands(input).empty());
  using compositors::hyprland::displayCommand;
  using compositors::hyprland::displayProblem;
  using compositors::hyprland::parseDisplays;
  const auto outputs = parseDisplays(
      R"([{"name":"DP-1","width":1920,"height":1080,"refreshRate":144,"x":350,"y":1440,"scale":1.25,"transform":2,"colorManagementPreset":"srgb","availableModes":["1920x1080@60.00Hz","1920x1080@144.00Hz","1920x1080@144.00Hz"]},{"name":"disconnected","disabled":true}])"
  );
  assert(outputs.size() == 1 && outputs[0].modes.size() == 2 && outputs[0].mode == "1920x1080@144.00");
  assert(parseDisplays("invalid").empty() && parseDisplays("{}").empty());
  HyprlandDisplayConfig display;
  display.output = "DP-1";
  display.managed = true;
  display.mode = "1920x1080@60.00";
  display.transform = 1;
  display.vrr = 2;
  display.colorMode = "wide";
  display.bitDepth = 10;
  display.sdrBrightness = 1.2F;
  assert(displayProblem(display, outputs[0]).empty());
  const auto displayLua = displayCommand(display, outputs[0]);
  assert(displayLua.find("position=\"350x1440\"") != std::string::npos);
  assert(displayLua.find("scale=1.25") != std::string::npos);
  assert(displayLua.find("vrr=2") != std::string::npos && displayLua.find("transform=1") != std::string::npos);
  assert(displayLua.find("cm=\"wide\"") != std::string::npos && displayLua.find("bitdepth=10") != std::string::npos);
  display.scale = 1.5F;
  display.positionManaged = true;
  display.x = -1280;
  display.y = 120;
  const auto layoutLua = displayCommand(display, outputs[0]);
  assert(layoutLua.find("position=\"-1280x120\"") != std::string::npos);
  assert(layoutLua.find("scale=1.5") != std::string::npos);
  const auto rotated = settings::displayRect(display, outputs[0]);
  assert(rotated.x == -1280 && rotated.y == 120 && rotated.width == 720 && rotated.height == 1280);
  auto adjacent = settings::snapDisplay({1913, 7, 1280, 720}, {{0, 0, 1920, 1080}}, 15);
  assert(adjacent.x == 1920 && adjacent.y == 0);
  auto negative = settings::snapDisplay({-1288, 5, 1280, 720}, {{0, 0, 1920, 1080}}, 15);
  assert(negative.x == -1280 && negative.y == 0);
  auto distant = settings::snapDisplay({1913, 3000, 1280, 720}, {{0, 0, 1920, 1080}}, 15);
  assert(distant.x == 1913 && distant.y == 3000);
  assert(!settings::overlaps(adjacent, {0, 0, 1920, 1080}));
  assert(settings::overlaps({1919, 0, 1280, 720}, {0, 0, 1920, 1080}));
  display.scale = .25F;
  assert(!displayProblem(display, outputs[0]).empty());
  display.scale = std::numeric_limits<float>::quiet_NaN();
  assert(!displayProblem(display, outputs[0]).empty());
  display.scale = 1.5F;
  display.x = -40000;
  assert(!displayProblem(display, outputs[0]).empty());
  display.x = -1280;
  ShellConfig displayShell, displayRestored;
  displayShell.hyprlandDisplays = {display};
  Diagnostics displayDiagnostics;
  readInto(writeTable(displayShell, shellSchema()), displayRestored, shellSchema(), "shell", displayDiagnostics);
  assert(displayRestored.hyprlandDisplays == displayShell.hyprlandDisplays);
  display.mode = "9999x9999@900";
  assert(!displayProblem(display, outputs[0]).empty() && displayCommand(display, outputs[0]).empty());
  display.mode.clear();
  display.colorMode = "unknown";
  assert(!displayProblem(display, outputs[0]).empty());
  display.colorMode.clear();
  display.bitDepth = 9;
  assert(!displayProblem(display, outputs[0]).empty());
  display.bitDepth = 0;
  display.managed = false;
  assert(displayCommand(display, outputs[0]).empty());
  std::istringstream layoutData(
      "! layout\n us English (US)\n gb English (UK)\n! variant\n dvorak us: English (Dvorak)\n malformed "
      "not-a-layout\n! option\n caps:none Disabled\n"
  );
  const auto catalog = parseKeyboardLayouts(layoutData);
  assert(catalog.size() == 3 && std::ranges::any_of(catalog, [](const auto& r) { return r.value == "us:dvorak"; }));
  HyprlandInputConfig keyboard;
  keyboard.keyboardManaged = true;
  keyboard.keyboardLayout2 = "gb";
  keyboard.keyboardLayout3 = "us:dvorak";
  keyboard.keyboardLayout4 = "us"; // Duplicate is skipped without disturbing variant alignment.
  keyboard.keyboardLayoutSwitch = HyprlandLayoutSwitch::SuperSpace;
  keyboard.keyboardCapsLock = HyprlandCapsLock::Escape;
  keyboard.keyboardRepeatRate = 42;
  keyboard.keyboardRepeatDelay = 275;
  keyboard.keyboardNumLock = true;
  const auto keyboardCommands = inputMotionCommands(keyboard);
  assert(keyboardCommands.find("kb_layout=\"us,gb,us\"") != std::string::npos);
  assert(keyboardCommands.find("kb_variant=\",,dvorak\"") != std::string::npos);
  assert(keyboardCommands.find("kb_options=\"grp:win_space_toggle,caps:escape\"") != std::string::npos);
  assert(keyboardCommands.find("repeat_rate=42,repeat_delay=275,numlock_by_default=true") != std::string::npos);
  assert(inputMotionPreset(keyboard, "subtle").keyboardLayout3 == "us:dvorak");
  ShellConfig keyboardShell, keyboardRestored;
  keyboardShell.hyprlandInput = keyboard;
  Diagnostics keyboardDiagnostics;
  readInto(writeTable(keyboardShell, shellSchema()), keyboardRestored, shellSchema(), "shell", keyboardDiagnostics);
  assert(keyboardRestored.hyprlandInput == keyboard);
  keyboard.keyboardCapsLock = HyprlandCapsLock::Control;
  assert(inputMotionCommands(keyboard).find("ctrl:nocaps") != std::string::npos);
  keyboard.keyboardCapsLock = HyprlandCapsLock::Normal;
  keyboard.keyboardLayoutSwitch = HyprlandLayoutSwitch::None;
  assert(inputMotionCommands(keyboard).find("kb_options=\"\"") != std::string::npos);
  keyboard.keyboardRepeatRate = -1;
  keyboard.keyboardRepeatDelay = 3000;
  assert(inputMotionCommands(keyboard).find("repeat_rate=0,repeat_delay=2000") != std::string::npos);
  keyboard.keyboardLayout2 = "invalid-layout";
  assert(inputMotionCommands(keyboard).empty());
  keyboard.keyboardLayout2 = "gb";
  keyboard.keyboardManaged = false;
  assert(inputMotionCommands(keyboard).empty());
  HyprlandInputConfig gestures;
  assert(compositors::hyprland::gestureInputCommands(gestures).empty());
  gestures.gesturesManaged = true;
  gestures.workspaceGestureFingers = HyprlandGestureFingers::Four;
  gestures.workspaceGestureSensitivity = 1.5F;
  gestures.workspaceGestureDistance = 450;
  gestures.workspaceGestureInvert = false;
  gestures.overviewGestureFingers = HyprlandGestureFingers::Three;
  gestures.overviewGestureDirection = HyprlandGestureDirection::Down;
  gestures.overviewGestureDistance = 140;
  const auto gestureCommands = compositors::hyprland::gestureInputCommands(gestures);
  assert(gestureCommands.find("fingers=4,direction=\"horizontal\",scale=1.5") != std::string::npos);
  assert(gestureCommands.find("workspace_swipe_distance=450,workspace_swipe_invert=false") != std::string::npos);
  assert(gestureCommands.find("fingers=3,direction=\"down\"") != std::string::npos);
  assert(gestureCommands.find("not e.cancelled and travel*1 >= 140") != std::string::npos);
  assert(inputMotionPreset(gestures, "subtle").workspaceGestureSensitivity == 1.5F);
  ShellConfig gestureShell, gestureRestored;
  gestureShell.hyprlandInput = gestures;
  Diagnostics gestureDiagnostics;
  readInto(writeTable(gestureShell, shellSchema()), gestureRestored, shellSchema(), "shell", gestureDiagnostics);
  assert(gestureRestored.hyprlandInput == gestures);
  gestures.workspaceGestureEnabled = false;
  assert(compositors::hyprland::gestureInputCommands(gestures).find("action=\"workspace\"") == std::string::npos);
  gestures.overviewGestureEnabled = false;
  assert(compositors::hyprland::gestureInputCommands(gestures).find("overview.toggle()") == std::string::npos);
  HyprlandInputConfig devices;
  devices.mouseManaged = true;
  devices.pointerSensitivity = .35F;
  devices.pointerAcceleration = HyprlandPointerAcceleration::Flat;
  devices.mouseLeftHanded = true;
  devices.mouseNaturalScroll = true;
  devices.mouseScrollFactor = 1.8F;
  devices.touchpadManaged = true;
  devices.touchpadNaturalScroll = true;
  devices.touchpadScrollFactor = .7F;
  devices.touchpadTapToClick = false;
  devices.touchpadTapAndDrag = false;
  devices.touchpadDragLock = HyprlandDragLock::Sticky;
  devices.touchpadTapMap = HyprlandTapMap::Lmr;
  devices.touchpadClickfinger = true;
  devices.touchpadMiddleEmulation = true;
  devices.touchpadDisableWhileTyping = false;
  devices.touchpadDragFingers = HyprlandDragFingers::Four;
  const auto deviceCommands = inputMotionCommands(devices);
  assert(deviceCommands.find("accel_profile=\"flat\"") != std::string::npos);
  assert(deviceCommands.find("tap_to_click=false") != std::string::npos);
  assert(deviceCommands.find("drag_lock=2") != std::string::npos);
  assert(deviceCommands.find("tap_button_map=\"lmr\"") != std::string::npos);
  assert(deviceCommands.find("drag_3fg=2") != std::string::npos);
  ShellConfig deviceShell, deviceRestored;
  deviceShell.hyprlandInput = devices;
  Diagnostics deviceDiagnostics;
  readInto(writeTable(deviceShell, shellSchema()), deviceRestored, shellSchema(), "shell", deviceDiagnostics);
  assert(deviceRestored.hyprlandInput == devices);
  const auto devicePreset = inputMotionPreset(devices, "subtle");
  assert(devicePreset.mouseManaged && devicePreset.touchpadManaged);
  assert(
      devicePreset.pointerAcceleration == devices.pointerAcceleration
      && devicePreset.touchpadDragFingers == devices.touchpadDragFingers
  );
  devices.mouseManaged = false;
  assert(inputMotionCommands(devices).find("sensitivity=") == std::string::npos);
  devices.touchpadManaged = false;
  assert(inputMotionCommands(devices).empty());
  readInto(
      toml::parse("[hyprland_input]\npointer_sensitivity=5.0\nmouse_scroll_factor=0.0\ntouchpad_scroll_factor=99.0\n"),
      deviceRestored, shellSchema(), "shell", deviceDiagnostics
  );
  assert(
      deviceRestored.hyprlandInput.pointerSensitivity == 1
      && deviceRestored.hyprlandInput.mouseScrollFactor == .1F
      && deviceRestored.hyprlandInput.touchpadScrollFactor == 5
  );
  input.scrollExcluded = "steam, app\"quoted\\name";
  input.edgeManaged = true;
  input.edgeTop = false;
  input.edgeFocus = 0;
  const auto subtle = inputMotionPreset(input, "subtle");
  assert(subtle.cursorManaged && subtle.scrollManaged);
  assert(subtle.cursorMode == HyprlandCursorMode::Tilt && subtle.tiltAngle == 25);
  assert(subtle.scrollExcluded == input.scrollExcluded && !subtle.edgeTop && subtle.edgeFocus == 0);
  const auto reduced = inputMotionPreset(subtle, "reduced");
  assert(reduced.cursorMode == HyprlandCursorMode::None && !reduced.shakeEnabled && !reduced.scrollEnabled);
  assert(reduced.edgeManaged && !reduced.edgeTop && reduced.scrollExcluded == input.scrollExcluded);
  assert(inputMotionPreset(input, "unknown") == input);
  const auto inputCommand = inputMotionCommands(subtle);
  assert(inputCommand.find("mode=\"tilt\"") != std::string::npos);
  assert(inputCommand.find("decel=0.9") != std::string::npos);
  assert(inputCommand.find("edges=\"lrb\"") != std::string::npos);
  assert(inputCommand.find("keyboard_focus=0") != std::string::npos);
  assert(inputCommand.find("app\\\"quoted\\\\name") != std::string::npos);
  assert(inputCommand.find("layer_pass") == std::string::npos);
  ShellConfig inputShell, inputRestored;
  inputShell.hyprlandInput = subtle;
  Diagnostics inputDiagnostics;
  readInto(writeTable(inputShell, shellSchema()), inputRestored, shellSchema(), "shell", inputDiagnostics);
  assert(inputRestored.hyprlandInput == subtle);
  readInto(
      toml::parse("[hyprland_input]\nscroll_decay=5.0\nshake_timeout=-5\nedge_focus=8"), inputRestored, shellSchema(),
      "shell", inputDiagnostics
  );
  assert(inputRestored.hyprlandInput.scrollDecay == .98F);
  assert(inputRestored.hyprlandInput.shakeTimeout == 100 && inputRestored.hyprlandInput.edgeFocus == 1);
  ShellConfig shell;
  Diagnostics diagnostics;
  readInto(
      toml::parse(R"(
[hyprland_appearance]
enabled = true
follow_theme = true
gaps_in = -3
gaps_out = 27
border_size = 99
rounding = 18
active_opacity = 0.01
inactive_opacity = 0.85
blur_size = 0
blur_passes = 99
shadow_range = 900
animations_enabled = false
custom_animations = true
animation_speed = 99
animation_easing = "snappy"
window_animation = "fade"
workspace_animation = "vertical"
)"),
      shell, shellSchema(), "shell", diagnostics
  );
  const auto& c = shell.hyprlandAppearance;
  assert(c.enabled && c.followTheme);
  assert(c.gapsIn == 0 && c.gapsOut == 27 && c.borderSize == 10);
  assert(c.activeOpacity == 0.2F && c.inactiveOpacity == 0.85F);
  assert(c.blurSize == 1 && c.blurPasses == 6 && c.shadowRange == 60);
  assert(!c.animationsEnabled);
  assert(c.customAnimations && c.animationSpeed == 3.0F);
  assert(c.animationEasing == HyprlandAnimationEasing::Snappy);
  assert(c.windowAnimation == HyprlandWindowAnimation::Fade);
  assert(c.workspaceAnimation == HyprlandWorkspaceAnimation::Vertical);
  ShellConfig restored;
  readInto(writeTable(shell, shellSchema()), restored, shellSchema(), "shell", diagnostics);
  assert(restored.hyprlandAppearance == c);

  using compositors::hyprland::appearanceCommand;
  assert(appearanceCommand({}, {}, {}).empty());
  const auto command = appearanceCommand(c, hex("#33b1ff"), hex("#161616"));
  assert(command.starts_with("repl hl.config("));
  assert(command.find("active_border=\"rgb(33b1ff)\"") != std::string::npos);
  assert(command.find("inactive_border=\"rgb(161616)\"") != std::string::npos);
  assert(command.find("animations={enabled=false}") != std::string::npos);
  auto untinted = c;
  untinted.followTheme = false;
  assert(appearanceCommand(untinted, {}, {}).find("active_border") == std::string::npos);

  assert(command.find("points={{0.16,1},{0.3,1}}") != std::string::npos);
  assert(command.find("style=\"popin 100%\"") != std::string::npos);
  assert(command.find("style=\"slidevert\"") != std::string::npos);
  auto motion = c;
  motion.customAnimations = false;
  assert(appearanceCommand(motion, {}, {}).find("hl.animation") == std::string::npos);
  motion.customAnimations = true;
  motion.animationSpeed = 2.0F;
  auto fast = appearanceCommand(motion, {}, {});
  assert(fast.find("leaf=\"windowsIn\",enabled=true,speed=2.0000") != std::string::npos);
  motion.animationSpeed = .5F;
  auto slow = appearanceCommand(motion, {}, {});
  assert(slow.find("leaf=\"windowsIn\",enabled=true,speed=8.0000") != std::string::npos);
  motion.windowAnimation = HyprlandWindowAnimation::Instant;
  motion.workspaceAnimation = HyprlandWorkspaceAnimation::Instant;
  auto instant = appearanceCommand(motion, {}, {});
  assert(instant.find("leaf=\"windowsIn\",enabled=false") != std::string::npos);
  assert(instant.find("leaf=\"fadeOut\",enabled=false") != std::string::npos);
  assert(instant.find("leaf=\"workspacesIn\",enabled=false") != std::string::npos);

  motion.windowAnimation = HyprlandWindowAnimation::Pop;
  motion.openingDuration = 800;
  motion.openingCurve.easing = HyprlandAnimationEasing::Spring;
  motion.openingCurve.stiffness = 300;
  motion.openingCurve.damping = 15;
  motion.closingCurve.easing = HyprlandAnimationEasing::Custom;
  motion.closingCurve.x1 = .2F;
  motion.closingCurve.y1 = 1.4F;
  auto custom = appearanceCommand(motion, {}, {});
  assert(custom.find("stiffness=300,dampening=15") != std::string::npos);
  assert(custom.find("spring=\"noctalia_opening\"") != std::string::npos);
  assert(custom.find("leaf=\"windowsIn\",enabled=true,speed=16.0000") != std::string::npos);
  assert(custom.find("points={{0.2,1.4},{0.36,1}}") != std::string::npos);
  motion.glassManaged = true;
  motion.cursorManaged = true;
  motion.overviewManaged = true;
  auto plugins = appearanceCommand(motion, {}, {});
  assert(plugins.find("if hl.plugin.hyprglass then") != std::string::npos);
  assert(plugins.find("if p.name==\"dynamic-cursors\"") != std::string::npos);
  assert(plugins.find("if hl.plugin.overview then") != std::string::npos);
  assert(plugins.find("panelColor=") == std::string::npos);
  auto overview = settings::softGlassOverview(motion);
  assert(overview.overviewManaged && overview.overviewStyleManaged);
  assert(overview.overviewHeight == motion.overviewHeight && overview.curve == motion.curve);
  const auto overviewCommand = appearanceCommand(overview, hex("#33b1ff"), hex("#161616"));
  assert(overviewCommand.find("panelColor=\"rgba(161616d1)\"") != std::string::npos);
  assert(overviewCommand.find("workspaceActiveBorder=\"rgba(33b1ffd9)\"") != std::string::npos);
  assert(overviewCommand.find("overrideAnimSpeed=3.5") != std::string::npos);
  assert(overviewCommand.find("centerAligned=true,disableBlur=false,dragAlpha=0.85") != std::string::npos);
  overview.overviewDuration = 0;
  assert(appearanceCommand(overview, {}, {}).find("overrideAnimSpeed=0") != std::string::npos);
  overview.overviewPanelColor = fixedColorSpec(hex("#112233"));
  assert(appearanceCommand(overview, {}, {}).find("panelColor=\"rgba(112233d1)\"") != std::string::npos);
  overview.overviewManaged = false;
  assert(appearanceCommand(overview, {}, {}).find("panelColor=") == std::string::npos);
  ShellConfig extra;
  extra.hyprlandAppearance = overview;
  extra.hyprlandAppearanceProfiles["My look"] = "rounding=12";
  ShellConfig extraRestored;
  readInto(writeTable(extra, shellSchema()), extraRestored, shellSchema(), "shell", diagnostics);
  assert(extraRestored.hyprlandAppearance == overview);
  assert(extraRestored.hyprlandAppearanceProfiles == extra.hyprlandAppearanceProfiles);

  auto effects = motion;
  effects.decorationEffectsManaged = true;
  effects.shadowColor = fixedColorSpec(hex("#446688"));
  effects.shadowInactiveColor = fixedColorSpec(hex("#112233"));
  effects.shadowOpacity = .5F;
  effects.shadowInactiveOpacity = .25F;
  effects.shadowOffsetX = -12;
  effects.shadowOffsetY = 8;
  effects.glowEnabled = true;
  effects.glowOpacity = .5F;
  effects.glowInactiveOpacity = 0;
  const auto effectCommand = appearanceCommand(effects, hex("#33b1ff"), hex("#161616"));
  assert(effectCommand.find("offset={-12,8}") != std::string::npos);
  assert(effectCommand.find("rgba(44668880)") != std::string::npos);
  assert(effectCommand.find("rgba(11223340)") != std::string::npos);
  assert(effectCommand.find("rgba(33b1ff80)") != std::string::npos);
  assert(effectCommand.find("rgba(33b1ff00)") != std::string::npos);
  assert(appearanceCommand(effects, hex("#ff8844"), {}).find("rgba(ff884480)") != std::string::npos);
  effects.decorationEffectsManaged = false;
  assert(appearanceCommand(effects, {}, {}).find("glow={") == std::string::npos);
  ShellConfig effectShell;
  effectShell.hyprlandAppearance = effects;
  ShellConfig effectRestored;
  readInto(writeTable(effectShell, shellSchema()), effectRestored, shellSchema(), "shell", diagnostics);
  assert(effectRestored.hyprlandAppearance == effects);
  readInto(
      toml::parse("[hyprland_appearance]\nshadow_power=99\nshadow_offset_x=-999\nglow_opacity=4\nglow_range=-10"),
      effectRestored, shellSchema(), "shell", diagnostics
  );
  assert(effectRestored.hyprlandAppearance.shadowPower == 4);
  assert(effectRestored.hyprlandAppearance.shadowOffsetX == -250);
  assert(effectRestored.hyprlandAppearance.glowOpacity == 1);
  assert(effectRestored.hyprlandAppearance.glowRange == 0);

  auto softGlass = settings::softGlassAppearance(motion);
  assert(softGlass.blurFocusManaged && softGlass.dimInactive && softGlass.blurPopups);
  assert(softGlass.roundingPower == 3 && softGlass.fullscreenOpacity == 1);
  assert(softGlass.glassManaged && softGlass.decorationEffectsManaged);
  assert(softGlass.cursorManaged == motion.cursorManaged && softGlass.overviewHeight == motion.overviewHeight);
  auto softCommand = appearanceCommand(softGlass, hex("#33b1ff"), hex("#161616"));
  assert(
      softCommand.find("rounding_power=3,fullscreen_opacity=1,dim_inactive=true,dim_strength=0.08") != std::string::npos
  );
  assert(
      softCommand.find("brightness=0.95,contrast=0.9,vibrancy=0.2,noise=0.015,popups=true,special=false")
      != std::string::npos
  );
  extra.hyprlandAppearance = softGlass;
  readInto(writeTable(extra, shellSchema()), extraRestored, shellSchema(), "shell", diagnostics);
  assert(extraRestored.hyprlandAppearance == softGlass);
  softGlass.blurFocusManaged = false;
  assert(appearanceCommand(softGlass, {}, {}).find("rounding_power") == std::string::npos);
  readInto(
      toml::parse(
          "[hyprland_appearance]\nrounding_power=99\nfullscreen_opacity=-1\nblur_brightness=4\nblur_contrast=-2\nblur_"
          "noise=3\nblur_vibrancy=-1\ndim_strength=2\ndim_special=-1\nblur_popups_ignorealpha=2"
      ),
      extraRestored, shellSchema(), "shell", diagnostics
  );
  const auto& bounded = extraRestored.hyprlandAppearance;
  assert(bounded.roundingPower == 10 && bounded.fullscreenOpacity == 0);
  assert(bounded.blurBrightness == 2 && bounded.blurContrast == 0);
  assert(bounded.blurNoise == 1 && bounded.blurVibrancy == 0);
  assert(bounded.dimStrength == 1 && bounded.dimSpecial == 0 && bounded.blurPopupsIgnorealpha == 1);

  HyprlandAppRule app;
  app.name = "quoted\"name";
  app.appClass = "org.example.App+[test]";
  app = settings::appAppearancePreset(app, "terminal");
  auto ruleCommand = compositors::hyprland::appRuleCommands({app});
  assert(ruleCommand.find("0.95 override 0.9 override 1 override") != std::string::npos);
  assert(ruleCommand.find("no_blur=false") != std::string::npos);
  assert(ruleCommand.find("tag=\"hyprglass_disabled\"") != std::string::npos);
  assert(ruleCommand.find(R"(^org\\.example\\.App\\+\\[test\\]$)") != std::string::npos);
  assert(ruleCommand.find(R"(quoted\"name)") != std::string::npos);
  extra.hyprlandAppRules = {app};
  ShellConfig rulesRestored;
  readInto(writeTable(extra, shellSchema()), rulesRestored, shellSchema(), "shell", diagnostics);
  assert(rulesRestored.hyprlandAppRules == extra.hyprlandAppRules);
  auto fullscreen = settings::appAppearancePreset(app, "fullscreen");
  fullscreen.name = "fullscreen";
  ruleCommand = compositors::hyprland::appRuleCommands({fullscreen, app});
  assert(ruleCommand.find("fullscreen=true") != std::string::npos);
  assert(ruleCommand.find("no_anim=true") != std::string::npos);
  assert(ruleCommand.find("quoted") < ruleCommand.find("noctalia-app-fullscreen"));
  auto reset = settings::appAppearancePreset(fullscreen, "reset");
  assert(reset.appClass == app.appClass && reset.scope == HyprlandRuleScope::Fullscreen);
  ruleCommand = compositors::hyprland::appRuleCommands({reset});
  assert(ruleCommand.find("opacity=") == std::string::npos && ruleCommand.find("tag=") == std::string::npos);
  app.enabled = false;
  assert(compositors::hyprland::appRuleCommands({app}).empty());
  app.enabled = true;
  app.appClass.clear();
  assert(compositors::hyprland::appRuleCommands({app}).empty());

  using compositors::hyprland::resolveAppearanceProfile;
  ShellConfig profiles;
  profiles.hyprlandAppearance = settings::softGlassAppearance({});
  profiles.hyprlandAppRules = {fullscreen};
  assert(resolveAppearanceProfile(profiles, true).appearance == profiles.hyprlandAppearance);
  profiles.hyprlandProfileSwitching.enabled = true;
  const auto darkLook = resolveAppearanceProfile(profiles, false);
  const auto lightLook = resolveAppearanceProfile(profiles, true);
  assert(!darkLook.appearance.glassLight && lightLook.appearance.glassLight);
  assert(darkLook.appearance.shadowOpacity == .55F && lightLook.appearance.shadowOpacity == .2F);
  assert(darkLook.appearance.blurBrightness == .95F && lightLook.appearance.blurBrightness == 1.04F);
  assert(lightLook.appearance.rounding == darkLook.appearance.rounding);
  assert(profiles.hyprlandAppRules == std::vector<HyprlandAppRule>{fullscreen});
  profiles.hyprlandAppearance = lightLook.appearance;
  assert(resolveAppearanceProfile(profiles, false).appearance.shadowOpacity == .55F);
  profiles.hyprlandAppearanceProfiles["Custom light"] = "rounding=19\nblur_brightness=1.17\n";
  profiles.hyprlandProfileSwitching.lightProfile = "Custom light";
  assert(resolveAppearanceProfile(profiles, true).appearance.rounding == 19);
  assert(resolveAppearanceProfile(profiles, true).appearance.enabled);
  profiles.hyprlandAppearance.enabled = false;
  assert(!resolveAppearanceProfile(profiles, true).appearance.enabled);
  profiles.hyprlandAppearanceProfiles["Custom light"] = "not valid toml";
  assert(resolveAppearanceProfile(profiles, true).fallback);
  assert(resolveAppearanceProfile(profiles, true).appearance == profiles.hyprlandAppearance);
  profiles.hyprlandAppearanceProfiles.erase("Custom light");
  assert(resolveAppearanceProfile(profiles, true).fallback);
  for (const auto invalid :
       {"rounding='invalid'", "blur_brightness=nan", "curve.x1='invalid'", "glass_enabled=1", "unknown_setting=true",
        "shadow_color=5", "glow_color='not-a-colour'", "animation_easing='unknown'"}) {
    profiles.hyprlandAppearanceProfiles["Custom light"] = invalid;
    assert(resolveAppearanceProfile(profiles, true).fallback);
    assert(resolveAppearanceProfile(profiles, true).appearance == profiles.hyprlandAppearance);
  }
  profiles.hyprlandAppearanceProfiles["Custom light"] = "blur_brightness=1\ncurve.x1=0.25";
  assert(!resolveAppearanceProfile(profiles, true).fallback);
  assert(resolveAppearanceProfile(profiles, true).appearance.blurBrightness == 1);
  std::ostringstream savedLook;
  savedLook << *writeTable(profiles, shellSchema())["hyprland_appearance"].as_table();
  profiles.hyprlandAppearanceProfiles["Custom light"] = savedLook.str();
  assert(!resolveAppearanceProfile(profiles, true).fallback);
  assert(resolveAppearanceProfile(profiles, true).appearance == profiles.hyprlandAppearance);
  ShellConfig profilesRestored;
  readInto(writeTable(profiles, shellSchema()), profilesRestored, shellSchema(), "shell", diagnostics);
  assert(profilesRestored.hyprlandProfileSwitching == profiles.hyprlandProfileSwitching);
  bool manualSwitchFound = false;
  for (const auto& [path, value] : settings::manualAppearanceOverrides(lightLook.appearance)) {
    if (path[1] == "hyprland_profile_switching") {
      assert(path.back() == "enabled" && !std::get<bool>(value));
      manualSwitchFound = true;
    }
    assert(path[1] != "hyprland_app_rules");
  }
  assert(manualSwitchFound);

  // Compare the whole persisted appearance, including nested curves and optional colours.
  // Every glass field must come from the shell; all other fields must come from the apps.
  const auto appearanceTable = [](const HyprlandAppearanceConfig& appearance) {
    ShellConfig shell;
    shell.hyprlandAppearance = appearance;
    return *writeTable(shell, shellSchema())["hyprland_appearance"].as_table();
  };
  const auto checkEffectiveAppearance = [&](const ShellConfig& shell) {
    for (const bool appsLight : {false, true}) {
      for (const bool shellLight : {false, true}) {
        auto expected = appearanceTable(resolveAppearanceProfile(shell, appsLight).appearance);
        const auto glass = appearanceTable(resolveAppearanceProfile(shell, shellLight).appearance);
        for (const auto& [key, value] : glass)
          if (key.str().starts_with("glass_"))
            expected.insert_or_assign(key, value);
        assert(appearanceTable(resolveEffectiveAppearance(shell, appsLight, shellLight)) == expected);
      }
    }
  };
  ShellConfig splitProfiles;
  splitProfiles.hyprlandAppearance = settings::softGlassAppearance({});
  checkEffectiveAppearance(splitProfiles); // Manual look ignores both modes.
  splitProfiles.hyprlandProfileSwitching.enabled = true;
  checkEffectiveAppearance(splitProfiles); // Built-ins, matching and split modes.
  auto customDark = darkLook.appearance;
  auto customLight = lightLook.appearance;
  customDark.glassManaged = customDark.glassEnabled = customDark.glassLayers = false;
  customLight.glassManaged = customLight.glassEnabled = customLight.glassLayers = true;
  customDark.glassLens = .1F;
  customLight.glassLens = .4F;
  customDark.glassOpacity = .6F;
  customLight.glassOpacity = .9F;
  customDark.rounding = 13;
  customLight.rounding = 23;
  customDark.openingCurve.x1 = .2F;
  customLight.openingCurve.x1 = .4F;
  customLight.shadowColor = fixedColorSpec(hex("#123456"));
  const auto darkTable = appearanceTable(customDark);
  const auto lightTable = appearanceTable(customLight);
  for (const auto& [key, value] : darkTable)
    if (key.str().starts_with("glass_")) {
      // A new glass field must also get distinct fixtures, or a missed copy could go unnoticed.
      auto changed = darkTable;
      changed.insert_or_assign(key, *lightTable.get(key));
      assert(changed != darkTable);
    }
  std::ostringstream savedDark, savedLight;
  savedDark << darkTable;
  savedLight << lightTable;
  splitProfiles.hyprlandAppearanceProfiles = {{"Dark", savedDark.str()}, {"Light", savedLight.str()}};
  splitProfiles.hyprlandProfileSwitching.darkProfile = "Dark";
  splitProfiles.hyprlandProfileSwitching.lightProfile = "Light";
  assert(!resolveAppearanceProfile(splitProfiles, false).fallback);
  assert(!resolveAppearanceProfile(splitProfiles, true).fallback);
  checkEffectiveAppearance(splitProfiles);
  splitProfiles.hyprlandAppearance.enabled = false;
  checkEffectiveAppearance(splitProfiles); // Master management stays authoritative.
  splitProfiles.hyprlandAppearanceProfiles["Light"] = "invalid toml";
  checkEffectiveAppearance(splitProfiles); // Invalid apps or glass profile falls back independently.
  splitProfiles.hyprlandAppearanceProfiles["Light"] = savedLight.str();
  splitProfiles.hyprlandAppearanceProfiles.erase("Dark");
  checkEffectiveAppearance(splitProfiles);
  splitProfiles.hyprlandProfileSwitching.enabled = false;
  checkEffectiveAppearance(splitProfiles);

  const auto overrides = settings::appearanceOverrides(motion);
  for (const auto& [path, value] : overrides) {
    if (path.back() == "rounding")
      assert(std::get<std::int64_t>(value) == motion.rounding);
    if (path.back() == "opening_duration")
      assert(std::get<double>(value) == 800);
    if (path.back() == "glass_managed")
      assert(std::get<bool>(value));
  }

  Config config;
  config.shell = shell;
  config.shell.hyprlandAppearance.animationsEnabled = true;
  const auto otherDesktop = settings::buildSettingsRegistry(config, nullptr);
  for (const auto& entry : otherDesktop)
    assert(entry.group != "hyprland-windows" && entry.group != "hyprland-animations");
  settings::RegistryEnvironment env;
  env.hyprlandAppearanceSupported = true;
  auto entries = settings::buildSettingsRegistry(config, nullptr, nullptr, env);
  int count = 0;
  for (const auto& entry : entries) {
    if (entry.group != "hyprland-windows" && entry.group != "hyprland-animations")
      continue;
    ++count;
    assert(entry.path.size() == 3 && entry.path[1] == "hyprland_appearance");
    if (entry.path.back() == "enabled")
      continue;
    assert(entry.visibleWhen && entry.visibleWhen(config));
    Config disabled = config;
    disabled.shell.hyprlandAppearance.enabled = false;
    assert(!entry.visibleWhen(disabled));
  }
  assert(count == 20);
  auto automaticConfig = config;
  automaticConfig.shell.hyprlandProfileSwitching.enabled = true;
  automaticConfig.shell.hyprlandAppearance.enabled = false;
  automaticConfig.shell.hyprlandInput = subtle;
  bool inputSectionFound = false;
  for (const auto& entry : settings::buildSettingsRegistry(automaticConfig, nullptr, nullptr, env)) {
    if (entry.section != settings::SettingsSection::InputMotion)
      continue;
    inputSectionFound = true;
    assert(entry.path[1] == "hyprland_input");
    if (entry.path.back() == "cursor_mode" || entry.path.back() == "scroll_decay" || entry.path.back() == "edge_focus")
      assert(entry.visibleWhen && entry.visibleWhen(automaticConfig));
  }
  assert(inputSectionFound);
  automaticConfig.shell.hyprlandWindowBehaviour = behaviour;
  int behaviourCount = 0;
  for (const auto& entry : settings::buildSettingsRegistry(automaticConfig, nullptr, nullptr, env)) {
    if (entry.section != settings::SettingsSection::WindowBehaviour)
      continue;
    ++behaviourCount;
    assert(entry.path.size() == 3 && entry.path[1] == "hyprland_window_behaviour");
    if (entry.path.back().ends_with("_managed"))
      assert(!entry.visibleWhen || entry.visibleWhen(automaticConfig));
    else {
      const bool followOnly = entry.path.back() == "focus_threshold" || entry.path.back() == "mouse_refocus";
      assert(entry.visibleWhen && entry.visibleWhen(automaticConfig) == !followOnly);
      auto disabled = automaticConfig;
      disabled.shell.hyprlandWindowBehaviour = {};
      assert(!entry.visibleWhen(disabled));
    }
  }
  assert(behaviourCount == 16);
  automaticConfig.shell.hyprlandTiling = tiling;
  int tilingCount = 0;
  for (const auto& entry : settings::buildSettingsRegistry(automaticConfig, nullptr, nullptr, env)) {
    if (entry.section != settings::SettingsSection::WorkspaceTiling)
      continue;
    ++tilingCount;
    assert(entry.path.size() == 3 && entry.path[1] == "hyprland_tiling");
    if (entry.path.back().ends_with("_managed"))
      assert(!entry.visibleWhen || entry.visibleWhen(automaticConfig));
    else {
      const bool hidden = entry.path.back() == "force_split"
          || entry.path.back() == "new_on_top"
          || entry.path.back() == "preserve_split"
          || entry.path.back() == "split_width_multiplier";
      assert(entry.visibleWhen && entry.visibleWhen(automaticConfig) == !hidden);
      auto disabled = automaticConfig;
      disabled.shell.hyprlandTiling = {};
      assert(!entry.visibleWhen(disabled));
    }
  }
  assert(tilingCount == 21);
  bool placementSectionFound = false;
  for (const auto& entry : settings::buildSettingsRegistry(automaticConfig, nullptr, nullptr, env)) {
    if (entry.section != settings::SettingsSection::AppPlacement)
      continue;
    placementSectionFound = true;
    assert(entry.path.size() == 2 && entry.path[1] == "hyprland_placement_rules");
    assert(!entry.visibleWhen || entry.visibleWhen(automaticConfig));
  }
  assert(placementSectionFound);
  automaticConfig.shell.hyprlandAppearance.enabled = true;
  for (const auto& entry : entries) {
    if (entry.path.size() >= 3
        && entry.path[1] == "hyprland_appearance"
        && entry.group != "hyprland-presets"
        && entry.path.back() != "enabled")
      assert(entry.visibleWhen && !entry.visibleWhen(automaticConfig));
    if (entry.group == "hyprland-app-rules" || entry.group == "hyprland-theme-profiles")
      assert(entry.visibleWhen && entry.visibleWhen(automaticConfig));
  }
  for (const auto& entry : entries) {
    if (entry.group != "hyprland-blur-focus")
      continue;
    Config preview = config;
    preview.shell.hyprlandAppearance = settings::softGlassAppearance({});
    assert(entry.visibleWhen && entry.visibleWhen(preview));
    preview.shell.hyprlandAppearance.blurFocusManaged = false;
    assert(entry.visibleWhen(preview) == (entry.path.back() == "blur_focus_managed"));
    preview.shell.hyprlandAppearance.enabled = false;
    assert(!entry.visibleWhen(preview));
  }
  std::cout << "PASS: Hyprland appearance schema, bounds, commands and settings visibility\n";
}
