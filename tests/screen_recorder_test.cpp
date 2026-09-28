#include "capture/screen_recorder.h"

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
  script("pactl", "#!/bin/sh\necho test_sink\n");
  script("xdg-user-dir", "#!/bin/sh\necho '" + root.string() + "'\n");
  script("wf-recorder", R"PY(#!/usr/bin/python3
import os,sys,signal,time
args=sys.argv[1:]
if '--audio=test_sink.monitor' not in args: sys.exit(9)
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
  bool reported = false, success = false;
  std::string result;
  recorder.completed = [&](bool ok, const std::string& text) {
    reported = true;
    success = ok;
    result = text;
  };
  bool ok = recorder.start("TEST-1", "0,0 320x240").empty() && recorder.active();
  ok = !recorder.start("TEST-1").empty() && ok;
  for (int i = 0; i < 200 && !std::filesystem::exists(root / "ready"); ++i)
    std::this_thread::sleep_for(10ms);
  recorder.stop();
  recorder.stop();
  auto drain = [&] {
    for (int i = 0; i < 600 && recorder.active(); ++i) {
      std::this_thread::sleep_for(10ms);
      TimerManager::instance().tick();
    }
  };
  drain();
  ok = ok && !recorder.active() && reported && success && std::filesystem::exists(result);
  reported = false;
  ::setenv("NOCTALIA_RECORD_TEST_FAIL", "1", 1);
  ok = recorder.start("TEST-1").empty() && ok;
  drain();
  ok = ok && !recorder.active() && reported && !success;
  recorder.shutdown();
  ::setenv("PATH", oldPath.c_str(), 1);
  std::filesystem::remove_all(root);
  if (!ok)
    std::cerr << "Recorder lifecycle, desktop audio, or failure reporting failed\n";
  return ok ? 0 : 1;
}
