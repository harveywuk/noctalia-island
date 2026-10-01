#include "pipewire/camera_device_scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace privacy {

  namespace {

    // PipeWire reports camera use routed through it with the consuming app's name.
    constexpr auto kSkippedProcesses = std::to_array<std::string_view>({
        "pipewire",
        "wireplumber",
        "pipewire-pulse",
        "pipewire-media-session",
    });

    // Interpreters are named after the script or module they run.
    constexpr auto kInterpreters = std::to_array<std::string_view>({
        "python", "python2", "python3", "node", "nodejs", "java", "ruby", "perl", "sh", "bash", "gjs",
    });

    std::string readFirstLine(const std::filesystem::path& path) {
      std::ifstream in(path);
      std::string line;
      std::getline(in, line);
      return line;
    }

    std::vector<std::string> readCmdline(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      std::vector<std::string> args;
      std::size_t start = 0;
      while (start < raw.size()) {
        const std::size_t end = raw.find('\0', start);
        args.emplace_back(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
          break;
        start = end + 1;
      }
      return args;
    }

    std::string basename(std::string_view path) {
      const auto slash = path.find_last_of('/');
      return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }

    std::string lower(std::string value) {
      std::ranges::transform(value, value.begin(), [](unsigned char c) { return std::tolower(c); });
      return value;
    }

    bool isInterpreter(std::string_view name) {
      // python3.12 and friends.
      const std::string base = lower(std::string(name.substr(0, name.find_first_of("0123456789."))));
      return std::ranges::contains(kInterpreters, std::string_view(base))
          || std::ranges::contains(kInterpreters, std::string_view(lower(std::string(name))));
    }

    std::string scriptName(const std::vector<std::string>& args) {
      for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-m" && i + 1 < args.size())
          return args[i + 1].substr(0, args[i + 1].find('.'));
        if (arg.empty() || arg.front() == '-')
          continue;
        std::string name = basename(arg);
        if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0)
          name.resize(dot);
        return name;
      }
      return {};
    }

    bool holdsVideoDevice(const std::filesystem::path& fdDir) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(fdDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code linkError;
        const auto target = std::filesystem::read_symlink(it->path(), linkError);
        if (!linkError && target.string().starts_with("/dev/video"))
          return true;
      }
      return false;
    }

  } // namespace

  std::string processDisplayName(const std::filesystem::path& procDir) {
    const auto args = readCmdline(procDir / "cmdline");
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink(procDir / "exe", ec);
    std::string name = !ec ? basename(exe.string()) : std::string{};
    // Deleted executables (after an upgrade) read as "name (deleted)".
    if (const auto suffix = name.find(" (deleted)"); suffix != std::string::npos)
      name.resize(suffix);
    if (name.empty())
      name = readFirstLine(procDir / "comm");
    if (name.empty() && !args.empty())
      name = basename(args.front());
    if (isInterpreter(name)) {
      if (auto script = scriptName(args); !script.empty())
        name = std::move(script);
    }
    return name;
  }

  std::vector<CameraDeviceUser> scanCameraDeviceUsers(const std::filesystem::path& procRoot, pid_t selfPid) {
    std::vector<CameraDeviceUser> users;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(procRoot, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string entry = it->path().filename().string();
      pid_t pid = 0;
      const auto [ptr, err] = std::from_chars(entry.data(), entry.data() + entry.size(), pid);
      if (err != std::errc{} || ptr != entry.data() + entry.size() || pid <= 0 || pid == selfPid)
        continue;
      const std::string comm = readFirstLine(it->path() / "comm");
      if (std::ranges::contains(kSkippedProcesses, std::string_view(comm)))
        continue;
      if (!holdsVideoDevice(it->path() / "fd"))
        continue;
      std::string name = processDisplayName(it->path());
      if (name.empty())
        continue;
      std::string binary = lower(name);
      if (std::ranges::any_of(users, [&](const CameraDeviceUser& user) { return user.binary == binary; }))
        continue;
      users.push_back(CameraDeviceUser{.pid = pid, .appName = std::move(name), .binary = std::move(binary)});
    }
    std::ranges::sort(users, {}, &CameraDeviceUser::binary);
    return users;
  }

} // namespace privacy
