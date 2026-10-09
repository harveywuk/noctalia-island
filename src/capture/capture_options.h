#pragma once

namespace capture {
  enum class RecordingAudio { Off, Desktop, Microphone };
  enum class Target { Region, Window, Monitor };

  struct LaunchOptions {
    bool recording = false;
    Target target = Target::Region;
    RecordingAudio audio = RecordingAudio::Desktop;
    int delaySeconds = 0;
  };
} // namespace capture
