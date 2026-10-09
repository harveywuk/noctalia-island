#include "capture/window_capture.h"

#include <iostream>
#include <nlohmann/json.hpp>

int main() {
  using nlohmann::json;
  const json tiled = {{"address", "0xa"}, {"mapped", true},     {"hidden", false},     {"visible", true},
                      {"at", {-200, 50}}, {"size", {400, 300}}, {"focusHistoryID", 0}, {"allowedOverFullscreen", true}};
  auto floating = tiled;
  floating["address"] = "0xb";
  floating["floating"] = true;
  floating["focusHistoryID"] = 2;
  floating["title"] = "Notes\nWeekly plan";
  floating["class"] = "notes";
  auto hidden = tiled;
  hidden["address"] = "0xc";
  hidden["visible"] = false;
  auto unmapped = tiled;
  unmapped["address"] = "0xd";
  unmapped["mapped"] = false;
  auto malformed = tiled;
  malformed["at"] = {1};
  auto zero = tiled;
  zero["size"] = {0, 20};
  auto invalidId = tiled;
  invalidId["address"] = "0xa; exec unwanted";
  auto windows = capture::parseHyprlandCaptureWindows(
      json::array({tiled, floating, hidden, unmapped, malformed, zero, invalidId}).dump()
  );
  bool ok = windows && windows->size() == 2;
  if (ok) {
    const auto* picked = capture::windowAt(*windows, 0, 100);
    ok = picked && picked->id == "b" && picked->bounds.x == -200;
    ok = ok && picked->title == "Notes Weekly plan";
    ok = ok && !capture::windowAt(*windows, 200, 100) && !capture::windowAt(*windows, 0, 350);
    floating["at"] = {300, 100};
    windows = capture::parseHyprlandCaptureWindows(json::array({tiled, floating}).dump());
    picked = capture::windowAt(*windows, 0, 100);
    ok = ok && picked && picked->id == "a";
    picked = capture::windowAt(*windows, 400, 150);
    ok = ok && picked && picked->id == "b";
  }
  auto oldIpc = tiled;
  oldIpc.erase("visible");
  ok = ok && !capture::parseHyprlandCaptureWindows(json::array({oldIpc}).dump());
  ok = ok && !capture::parseHyprlandCaptureWindows("not json") && !capture::parseHyprlandCaptureWindows("{}");
  windows = capture::parseHyprlandCaptureWindows("[]");
  ok = ok && windows && windows->empty();
  floating["title"] = "";
  windows = capture::parseHyprlandCaptureWindows(json::array({floating}).dump());
  ok = ok && windows && windows->size() == 1 && windows->front().title == "notes";
  if (!ok)
    std::cerr << "Window visibility, stacking, geometry or malformed IPC handling failed\n";
  return ok ? 0 : 1;
}
