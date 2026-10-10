#include "shell/assistant/assistant_audio_level.h"
#include "tests/test_check.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sndfile.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
  AssistantAudioLevel input;
  TEST_CHECK(input.level() == 0);
  input.capture(std::array{0.0F, 0.0F, 0.0F});
  TEST_CHECK(input.level() == 0);
  input.capture(std::array{0.0001F, -0.0001F});
  TEST_CHECK(input.level() == 0);
  input.capture(std::array{0.03F, -0.03F});
  const float quiet = input.level();
  TEST_CHECK(quiet > 0 && quiet < 1);
  input.capture(std::array{0.3F, -0.3F});
  TEST_CHECK(input.level() > quiet && input.level() <= 1);
  input.capture(std::array{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()});
  TEST_CHECK(input.level() == 0);
  input.capture(std::array{0.3F});
  std::this_thread::sleep_for(std::chrono::milliseconds(230));
  TEST_CHECK(input.level() == 0); // Disconnected or stalled capture must settle.

  char temporary[] = "/tmp/noctalia-envelope-XXXXXX";
  TEST_CHECK(mkdtemp(temporary));
  const auto file = std::filesystem::path(temporary) / "speech.wav";
  SF_INFO info{};
  info.samplerate = 16000;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto* wav = sf_open(file.c_str(), SFM_WRITE, &info);
  TEST_CHECK(wav);
  std::vector<float> tone(32000);
  for (std::size_t i = 0; i < tone.size(); ++i)
    tone[i] = 0.25F * std::sin(static_cast<float>(i) * 0.1F);
  TEST_CHECK(
      sf_writef_float(wav, tone.data(), static_cast<sf_count_t>(tone.size())) == static_cast<sf_count_t>(tone.size())
  );
  TEST_CHECK(sf_close(wav) == 0);

  AssistantAudioLevel output;
  TEST_CHECK(output.loadPlayback(file));
  TEST_CHECK(output.level() == 0); // Process launch alone does not start animation.
  output.playbackEvent("stream state changed paused -> stream");
  TEST_CHECK(output.level() == 0);
  output.playbackEvent("ing\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  TEST_CHECK(output.level() > 0.5F);
  output.playbackEvent("stream state changed streaming -> paused\n");
  TEST_CHECK(output.level() == 0);
  output.playbackEvent("stream state changed paused -> streaming\n");
  TEST_CHECK(output.level() > 0.5F);
  output.playbackEvent("stream drained\n");
  TEST_CHECK(output.level() == 0);
  TEST_CHECK(!AssistantAudioLevel{}.loadPlayback(file.parent_path() / "missing.wav"));
  std::filesystem::remove_all(temporary);
  return 0;
}
