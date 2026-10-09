#include "capture/screen_recorder.h"

#include "core/process/process.h"
#include "util/string_utils.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
using namespace std::chrono_literals;

ScreenRecorder& ScreenRecorder::instance() {
  static ScreenRecorder recorder;
  return recorder;
}

std::string
ScreenRecorder::start(const std::string& output, const std::string& geometry, capture::RecordingAudio audio) {
  if (active())
    return "A recording is already running";
  if (!process::commandExists("wf-recorder"))
    return "Install wf-recorder to record video";
  // Resolve the requested source explicitly. Silent video does not need an audio server.
  std::string source;
  if (audio != capture::RecordingAudio::Off) {
    const bool desktop = audio == capture::RecordingAudio::Desktop;
    auto device = process::runSyncWithTimeout({"pactl", desktop ? "get-default-sink" : "get-default-source"}, 2s);
    source = StringUtils::trim(device.out);
    if (!device || source.empty() || (!desktop && source.ends_with(".monitor")))
      return desktop ? "Cannot find the desktop audio output" : "Cannot find a microphone input";
    if (desktop)
      source += ".monitor";
  }
  auto directoryResult = process::runSyncWithTimeout({"xdg-user-dir", "VIDEOS"}, 2s);
  std::filesystem::path directory = StringUtils::trim(directoryResult.out);
  if (!directoryResult || !directory.is_absolute()) {
    const char* home = std::getenv("HOME");
    if (!home)
      return "Cannot locate the Videos directory";
    directory = std::filesystem::path(home) / "Videos";
  }
  directory /= "Recordings";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return "Cannot create recording directory: " + error.message();
  const auto now = std::chrono::system_clock::now();
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  m_path = directory / std::format("Recording-{}-{}.mp4", stamp, ::getpid());
  m_log = m_path;
  m_log += ".log";
  // NVENC keeps gaming capture off the CPU. Software is used when no NVIDIA GPU is present.
  const bool nvidia = std::filesystem::exists("/proc/driver/nvidia/version");
  std::vector<std::string> args{
      "wf-recorder",
      "--overwrite",
      "--no-dmabuf",
      "-o",
      output,
      "-r",
      "60",
      "-c",
      nvidia ? "h264_nvenc" : "libx264",
      "-x",
      "yuv420p",
      "-F",
      "pad=ceil(iw/2)*2:ceil(ih/2)*2",
      "-f",
      m_path.string()
  };
  if (!source.empty())
    args.insert(args.end(), {"--audio=" + source, "-C", "aac"});
  if (nvidia) {
    args.insert(args.end(), {"-p", "preset=p4", "-p", "cq=20"});
  } else {
    args.insert(args.end(), {"-p", "preset=veryfast", "-p", "crf=20"});
  }
  if (!geometry.empty())
    args.insert(args.end(), {"-g", geometry});
  std::vector<char*> argv;
  for (auto& arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, m_log.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t pid = -1;
  const int result = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result)
    return "Cannot start recorder: " + std::string(std::strerror(result));
  m_pid = pid;
  m_stopping = false;
  m_started = std::chrono::steady_clock::now();
  ++m_sessionId;
  m_poll.startRepeating(200ms, [this] { poll(); });
  if (activeChanged)
    activeChanged(true);
  return {};
}

void ScreenRecorder::stop() {
  if (!active() || m_stopping)
    return;
  m_stopping = true;
  m_stopRequested = std::chrono::steady_clock::now();
  ::kill(m_pid, SIGINT); // Let the encoder flush audio and finish the MP4 container.
}

std::chrono::seconds ScreenRecorder::elapsed() const {
  if (!active())
    return 0s;
  return std::chrono::duration_cast<std::chrono::seconds>(
      (m_stopping ? m_stopRequested : std::chrono::steady_clock::now()) - m_started
  );
}

std::string ScreenRecorder::label() const {
  if (!active())
    return {};
  if (m_stopping)
    return "Saving…";
  const auto seconds = elapsed().count();
  return std::format("REC {:02}:{:02}  ■", seconds / 60, seconds % 60);
}

void ScreenRecorder::poll() {
  if (!active())
    return;
  int status = 0;
  const auto result = ::waitpid(m_pid, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) {
    if (m_stopping && std::chrono::steady_clock::now() - m_stopRequested > 15s)
      ::kill(m_pid, SIGKILL);
    return;
  }
  const auto duration = elapsed();
  m_pid = -1;
  m_stopping = false;
  m_poll.stop();
  if (activeChanged)
    activeChanged(false);
  std::error_code error;
  const auto bytes = std::filesystem::file_size(m_path, error);
  const bool success = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !error && bytes > 0;
  if (success)
    std::filesystem::remove(m_log, error);
  if (completed)
    completed({
        .success = success,
        .path = m_path,
        .duration = duration,
        .bytes = success ? bytes : 0,
        .error = success ? std::string{} : "Recording failed; details: " + m_log.string(),
    });
}

void ScreenRecorder::shutdown() {
  completed = {};
  stop();
  for (int i = 0; active() && i < 100; ++i) {
    poll();
    if (active())
      std::this_thread::sleep_for(50ms);
  }
  if (active()) {
    ::kill(m_pid, SIGKILL);
    ::waitpid(m_pid, nullptr, 0);
    m_pid = -1;
    if (activeChanged)
      activeChanged(false);
  }
  m_poll.stop();
  activeChanged = {};
}
