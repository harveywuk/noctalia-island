#include "compositors/hyprland/hyprland_appearance.h"
#include "compositors/hyprland/hyprland_runtime.h"

#include <algorithm>
#include <format>

namespace compositors::hyprland {
  std::string gestureInputCommands(const HyprlandInputConfig& c) {
    if (!c.gesturesManaged)
      return {};
    const int workspaceFingers = c.workspaceGestureFingers == HyprlandGestureFingers::Four ? 4 : 3;
    const int overviewFingers = c.overviewGestureFingers == HyprlandGestureFingers::Four ? 4 : 3;
    const auto direction = c.overviewGestureDirection == HyprlandGestureDirection::Down ? "down" : "up";
    const float sensitivity = std::clamp(c.workspaceGestureSensitivity, .25F, 3.F);
    const int distance = std::clamp(c.workspaceGestureDistance, 100, 1000);
    const int threshold = std::clamp(c.overviewGestureDistance, 30, 500);
    const auto signature = std::format(
        "{}:{}:{}:{}:{}:{}:{}:{}:{}", c.workspaceGestureEnabled, workspaceFingers, sensitivity, distance,
        c.workspaceGestureInvert, c.overviewGestureEnabled, overviewFingers, direction, threshold
    );
    // Lua state resets on compositor reload. In one state, remove only our own
    // rules, including their exact scale (required by Hyprland's unset API).
    std::string out = "; do local key=" + luaStringLiteral(signature) + R"lua(
      local overview = hl.plugin.overview
      key = key .. ':' .. tostring(overview ~= nil)
      local old = _G.__noctalia_touchpad_gestures
      if not old or old.key ~= key then
        if old then
          for _, rule in ipairs(old.rules) do
            hl.gesture({fingers=rule.fingers,direction=rule.direction,scale=rule.scale or 1,action="unset"})
          end
        end
        local rules = {}
        local function add(rule) hl.gesture(rule); table.insert(rules, rule) end
)lua";
    out += std::format(
        "hl.config({{gestures={{workspace_swipe_distance={},workspace_swipe_invert={}}}}})\n", distance,
        c.workspaceGestureInvert
    );
    // Native bindings replace Hyprspace's older event interception while managed.
    out += "if overview then hl.config({plugin={overview={disableGestures=true}}}) end\n";
    if (c.workspaceGestureEnabled)
      out += std::format(
          "add({{fingers={},direction=\"horizontal\",scale={},action=\"workspace\"}})\n", workspaceFingers, sensitivity
      );
    if (c.overviewGestureEnabled) {
      out += "if overview then local travel=0\n";
      out += std::format("add({{fingers={},direction={},action={{", overviewFingers, luaStringLiteral(direction));
      out += "start=function() travel=0 end, update=function(e) travel=travel+e.delta.y end,";
      out += std::format(
          "finish=function(e) if not e.cancelled and travel*{} >= {} and hl.plugin.overview then "
          "hl.plugin.overview.toggle() end end",
          direction == std::string_view("down") ? 1 : -1, threshold
      );
      out += "}}) end\n";
    }
    out += "_G.__noctalia_touchpad_gestures={key=key,rules=rules}\n end end";
    return out;
  }
} // namespace compositors::hyprland
