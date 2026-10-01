#include "pipewire/camera_device_scanner.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  // A fake /proc/<pid> with comm, cmdline (NUL-separated), exe and fd symlinks.
  void process(
      const fs::path& root, int pid, std::string_view comm, std::string_view exe, std::string_view cmdline,
      std::initializer_list<std::string_view> fds
  ) {
    const fs::path dir = root / std::to_string(pid);
    fs::create_directories(dir / "fd");
    std::ofstream(dir / "comm") << comm << '\n';
    std::ofstream(dir / "cmdline", std::ios::binary) << cmdline;
    if (!exe.empty())
      fs::create_symlink(exe, dir / "exe");
    int fd = 3;
    for (const auto target : fds)
      fs::create_symlink(target, dir / "fd" / std::to_string(fd++));
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("camera-scan-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  using namespace std::string_view_literals;
  process(root, 100, "firefox", "/usr/lib/firefox/firefox", "firefox\0"sv, {"/dev/null", "/dev/video0"});
  // A second process of the same app is reported once.
  process(root, 101, "firefox", "/usr/lib/firefox/firefox", "firefox\0-contentproc\0"sv, {"/dev/video0"});
  process(root, 200, "python3", "/usr/bin/python3.12", "python\0-m\0nvbroadcast\0"sv, {"/dev/video0", "/dev/video10"});
  process(root, 300, "pipewire", "/usr/bin/pipewire", "pipewire\0"sv, {"/dev/video0"});
  process(root, 400, "kitty", "/usr/bin/kitty", "kitty\0"sv, {"/dev/pts/1"});
  process(root, 500, "noctalia", "/usr/bin/noctalia", "noctalia\0"sv, {"/dev/video0"});
  process(root, 600, "Discord", "/opt/discord/Discord (deleted)", "/opt/discord/Discord\0"sv, {"/dev/video1"});
  fs::create_directories(root / "self");

  const auto users = privacy::scanCameraDeviceUsers(root, 500);
  TEST_CHECK(users.size() == 3);
  TEST_CHECK(users[0].appName == "Discord" && users[0].binary == "discord" && users[0].pid == 600);
  TEST_CHECK(users[1].appName == "firefox" && (users[1].pid == 100 || users[1].pid == 101));
  TEST_CHECK(users[2].appName == "nvbroadcast" && users[2].binary == "nvbroadcast");

  // Scripts run by path are named after the script, without its extension.
  process(root, 700, "bash", "/usr/bin/bash", "bash\0/home/user/bin/webcam-check.sh\0"sv, {});
  TEST_CHECK(privacy::processDisplayName(root / "700") == "webcam-check");

  TEST_CHECK(privacy::scanCameraDeviceUsers(root / "missing").empty());
  fs::remove_all(root);
  return 0;
}
