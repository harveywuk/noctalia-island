#include "shell/assistant/assistant_actions.h"
#include "tests/test_check.h"

#include <iostream>

int main() {
  using Actions = AssistantActions;
  using Kind = Actions::Kind;
  const auto check = [](const char* text, Kind kind, int amount = 0, const char* mode = "") {
    const auto action = Actions::parse(text);
    if (!action || action->kind != kind || action->amount != amount || action->mode != mode)
      std::cerr << "Unexpected parse result for: " << text << '\n';
    TEST_CHECK(action.has_value());
    TEST_CHECK(action->kind == kind);
    TEST_CHECK(action->amount == amount);
    TEST_CHECK(action->mode == mode);
  };
  check("Set a timer for ten minutes.", Kind::Timer, 600);
  check("Please start a twenty-five minute timer!", Kind::Timer, 1500);
  check("Could you set a timer for 1h30m please?", Kind::Timer, 5400);
  check("Start a timer for one hour and thirty minutes", Kind::Timer, 5400);
  check("Set a timer for half an hour", Kind::Timer, 1800);
  check("timer 1s", Kind::Timer, 1);
  check("timer 24 hours", Kind::Timer, 86400);
  check("How long is left?", Kind::TimerStatus);
  check("How much time is left on my timer?", Kind::TimerStatus);
  check("timer status", Kind::TimerStatus);
  check("Please pause the timer", Kind::PauseTimer);
  check("Can you resume my timer?", Kind::ResumeTimer);
  check("Cancel timer", Kind::CancelTimer);
  check("Stop the timer", Kind::CancelTimer);
  check("Can you please pause the music?", Kind::PauseMedia);
  check("Resume my music", Kind::ResumeMedia);
  check("Play media", Kind::ResumeMedia);
  check("What's playing?", Kind::MediaStatus);
  check("What’s playing?", Kind::MediaStatus);
  check("What song is playing?", Kind::MediaStatus);
  check("Skip this track", Kind::NextMedia);
  check("Next song", Kind::NextMedia);
  check("Play the next track", Kind::NextMedia);
  check("What’s my battery level?", Kind::BatteryStatus);
  check("How much battery is left?", Kind::BatteryStatus);
  check("Which Focus is active?", Kind::FocusStatus);
  check("Which focus mode is active", Kind::FocusStatus);
  check("Set volume to 30%", Kind::Volume, 30);
  check("Set the volume to thirty percent.", Kind::Volume, 30);
  check("Set volume to zero percent", Kind::Volume, 0);
  check("Set volume to one hundred percent", Kind::Volume, 100);
  check("Enable Focus", Kind::Focus, 0, "work");
  check("Turn on gaming focus mode", Kind::Focus, 0, "gaming");
  check("Disable Focus", Kind::Focus, 0, "off");
  check("Set focus to auto", Kind::Focus, 0, "auto");
  for (const auto* text :
       {"timer 0m",
        "timer -1m",
        "timer 25h",
        "timer 24h1s",
        "timer 999999999999999999999h",
        "timer 1.5h",
        "timer ten bananas",
        "timer 10m and pause the music",
        "timer 1h and",
        "timer 10m 2h",
        "timer 10m 5m",
        "Set a timer",
        "Set volume to 101%",
        "Set volume to -5%",
        "Set volume to NaN",
        "Set volume to thirty percent and enable focus",
        "Enable Focus and pause the music",
        "Pause the timer and skip this track",
        "Cancel the timer in ten minutes",
        "Skip this track tomorrow"})
    check(text, Kind::Invalid);
  for (const auto* text :
       {"How do I set a timer for ten minutes?", "What does enable Focus mean?", "Don't pause the music",
        "Explain the command 'set volume to 30%'", "Say pause the music", "If I say enable focus, what happens?",
        "\"Pause the music\"", "`Set volume to 30%`", "Turn volume down tomorrow", "Play a song called Pause the Music",
        "Don't cancel the timer", "How do I pause the timer?", "Explain what timer status means",
        "What is playing a song like?", "How long is left in the film?", "What should my battery level be?",
        "If I skip this track, what happens?", "Pause it"})
    TEST_CHECK(!Actions::parse(text));
  return 0;
}
