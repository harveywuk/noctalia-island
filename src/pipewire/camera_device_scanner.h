#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <sys/types.h>

namespace privacy {

  // A process holding a V4L2 video device (/dev/videoN) open. Most apps (browsers without
  // PipeWire camera support, Discord, Zoom, OBS) open the webcam directly, so PipeWire's graph
  // never sees them.
  struct CameraDeviceUser {
    pid_t pid = 0;
    // Display name: the executable, or the script/module an interpreter runs.
    std::string appName;
    // Lower-case executable (or script) name, used to find the app's windows.
    std::string binary;

    bool operator==(const CameraDeviceUser&) const = default;
  };

  // One entry per app (first process found), sorted by name. PipeWire's own daemons and
  // `selfPid` are skipped: camera use routed through PipeWire is reported from its graph.
  [[nodiscard]] std::vector<CameraDeviceUser>
  scanCameraDeviceUsers(const std::filesystem::path& procRoot = "/proc", pid_t selfPid = 0);

  // The name a process is shown as, from its comm, exe and command line.
  [[nodiscard]] std::string processDisplayName(const std::filesystem::path& procDir);

} // namespace privacy
