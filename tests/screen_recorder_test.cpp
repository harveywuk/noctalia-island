#include "capture/screen_recorder.h"
#include "notification/notification_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace std::chrono_literals;
int main() {
  char base[] = "/tmp/noctalia-recorder-test-XXXXXX";
  const char* made = ::mkdtemp(base);
  if (!made)
    return 1;
  const std::filesystem::path root(made);
  auto script = [&](const char* name, const std::string& text) {
    const auto path = root / name;
    std::ofstream(path) << text;
    ::chmod(path.c_str(), 0700);
  };
  script("pactl", R"SH(#!/bin/sh
[ -n "$NOCTALIA_RECORD_TEST_NO_AUDIO" ] && exit 1
if [ "$1" = get-default-sink ]; then echo test_sink
else echo "${NOCTALIA_RECORD_TEST_SOURCE:-test_mic}"; fi
)SH");
  script("xdg-user-dir", "#!/bin/sh\necho '" + root.string() + "'\n");
  script("wf-recorder", R"PY(#!/usr/bin/python3
import os,sys,signal,time
args=sys.argv[1:]
expected=os.getenv('NOCTALIA_RECORD_TEST_AUDIO','test_sink.monitor')
audio=[arg for arg in args if arg.startswith('--audio')]
if audio != (['--audio='+expected] if expected else []): sys.exit(9)
if ('-C' in args) != bool(expected): sys.exit(10)
if os.getenv('NOCTALIA_RECORD_TEST_FAIL'): sys.exit(3)
path=args[args.index('-f')+1]
def done(*_):
    with open(path,'wb') as f: f.write(b'finished video')
    sys.exit(0)
signal.signal(signal.SIGINT,done)
with open(os.environ['NOCTALIA_RECORD_TEST_READY'],'w') as f: f.write('ready')
while True: time.sleep(.02)
)PY");
  const std::string oldPath = std::getenv("PATH");
  ::setenv("PATH", (root.string() + ":" + oldPath).c_str(), 1);
  ::setenv("NOCTALIA_RECORD_TEST_READY", (root / "ready").c_str(), 1);
  auto& recorder = ScreenRecorder::instance();
  NotificationManager notifications;
  notifications.configureFocus(FocusConfig{.whileRecording = true});
  notifications.selectFocus("work");
  std::vector<bool> activeChanges;
  recorder.activeChanged = [&](bool active) {
    activeChanges.push_back(active);
    notifications.setRecordingActive(active);
  };
  bool reported = false, success = false;
  std::string result;
  ScreenRecorder::Result completion;
  bool restoredBeforeCompletion = true;
  recorder.completed = [&](const ScreenRecorder::Result& saved) {
    reported = true;
    success = saved.success;
    result = saved.path.string();
    completion = saved;
    restoredBeforeCompletion = restoredBeforeCompletion && notifications.focusId() == "work";
  };
  bool ok = recorder.start("TEST-1", "0,0 320x240").empty() && recorder.active();
  ok = ok && notifications.focusId() == "recording" && activeChanges == std::vector<bool>{true};
  const auto firstSession = recorder.sessionId();
  ok = ok && firstSession > 0 && !recorder.stopping() && recorder.elapsed() >= 0s;
  ok = !recorder.start("TEST-1").empty() && ok;
  for (int i = 0; i < 200 && !std::filesystem::exists(root / "ready"); ++i)
    std::this_thread::sleep_for(10ms);
  std::this_thread::sleep_for(1100ms);
  recorder.stop();
  const auto stoppedAt = recorder.elapsed();
  ok = ok && notifications.focusId() == "recording"; // Keep Focus until the recorder has really exited.
  ok = ok && recorder.stopping();
  std::this_thread::sleep_for(20ms);
  ok = ok && recorder.elapsed() == stoppedAt;
  recorder.stop();
  auto drain = [&] {
    for (int i = 0; i < 600 && recorder.active(); ++i) {
      std::this_thread::sleep_for(10ms);
      TimerManager::instance().tick();
    }
  };
  drain();
  ok = ok && !recorder.active() && reported && success && std::filesystem::exists(result);
  ok = ok
      && completion.duration == stoppedAt
      && completion.duration >= 1s
      && completion.bytes == 14
      && completion.error.empty();
  ok = ok && !recorder.stopping() && recorder.elapsed() == 0s;
  reported = false;
  ::setenv("NOCTALIA_RECORD_TEST_FAIL", "1", 1);
  ok = recorder.start("TEST-1").empty() && ok;
  ok = ok && recorder.sessionId() != firstSession && !recorder.stopping();
  drain();
  ok = ok && !recorder.active() && reported && !success;
  ok = ok && completion.bytes == 0 && !completion.error.empty();
  ::unsetenv("NOCTALIA_RECORD_TEST_FAIL");
  // Verify both the selected source and the absence of all audio arguments for silent video.
  for (const auto mode : {capture::RecordingAudio::Off, capture::RecordingAudio::Microphone}) {
    const bool silent = mode == capture::RecordingAudio::Off;
    ::setenv("NOCTALIA_RECORD_TEST_AUDIO", silent ? "" : "test_mic", 1);
    if (silent)
      ::setenv("NOCTALIA_RECORD_TEST_NO_AUDIO", "1", 1);
    else
      ::unsetenv("NOCTALIA_RECORD_TEST_NO_AUDIO");
    std::filesystem::remove(root / "ready");
    reported = false;
    ok = recorder.start("TEST-1", {}, mode).empty() && ok;
    for (int i = 0; i < 200 && !std::filesystem::exists(root / "ready"); ++i)
      std::this_thread::sleep_for(10ms);
    ok = std::filesystem::exists(root / "ready") && ok;
    recorder.stop();
    drain();
    ok = !recorder.active() && reported && success && ok;
  }
  // Never silently substitute desktop audio when the user explicitly chooses a microphone.
  ::setenv("NOCTALIA_RECORD_TEST_SOURCE", "test_sink.monitor", 1);
  ok = !recorder.start("TEST-1", {}, capture::RecordingAudio::Microphone).empty() && !recorder.active() && ok;
  ::setenv("NOCTALIA_RECORD_TEST_NO_AUDIO", "1", 1);
  ok = !recorder.start("TEST-1").empty() && !recorder.active() && ok;
  ::unsetenv("NOCTALIA_RECORD_TEST_SOURCE");
  ::unsetenv("NOCTALIA_RECORD_TEST_NO_AUDIO");
  ::unsetenv("NOCTALIA_RECORD_TEST_AUDIO");
  ::unsetenv("NOCTALIA_RECORD_TEST_READY");
  ok = ok && restoredBeforeCompletion && notifications.focusId() == "work";
  ok = ok && activeChanges == std::vector<bool>({true, false, true, false, true, false, true, false});
  if (!ok)
    std::cerr << "Normal/failed recording lifecycle failed before spawn-error checks\n";

  // An executable with a missing interpreter fails at spawn, without activating Focus.
  // Restrict PATH so posix_spawnp cannot fall through to the installed recorder.
  script("wf-recorder", "#!/noctalia-nonexistent-interpreter\n");
  ::setenv("PATH", root.c_str(), 1);
  ok = !recorder.start("TEST-1", {}, capture::RecordingAudio::Off).empty() && !recorder.active() && ok;
  ok = ok && activeChanges.size() == 8 && notifications.focusId() == "work";
  ::setenv("PATH", (root.string() + ":" + oldPath).c_str(), 1);

  // Shutdown must release the temporary policy even though completion previews are disabled.
  script("wf-recorder", "#!/bin/sh\nexec sleep 300\n");
  const auto shutdownStartError = recorder.start("TEST-1", {}, capture::RecordingAudio::Off);
  if (!shutdownStartError.empty())
    std::cerr << "Shutdown fixture could not start: " << shutdownStartError << '\n';
  ok = shutdownStartError.empty() && ok;
  ok = ok && notifications.focusId() == "recording";
  recorder.shutdown();
  ok = ok && !recorder.active() && notifications.focusId() == "work" && activeChanges.size() == 10;
  if (!ok)
    std::cerr << "Lifecycle changes: " << activeChanges.size() << ", Focus: " << notifications.focusId() << '\n';
  ::setenv("PATH", oldPath.c_str(), 1);
  std::filesystem::remove_all(root);
  if (!ok)
    std::cerr << "Recorder lifecycle, audio selection, or failure reporting failed\n";
  return ok ? 0 : 1;
}
