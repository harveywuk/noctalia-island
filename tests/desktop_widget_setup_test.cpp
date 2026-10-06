#include "shell/desktop/desktop_widget_setup.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  char path[] = "/tmp/noctalia-widget-setup-XXXXXX";
  expect(mkdtemp(path) != nullptr, "fixture directory created");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{path};
  DesktopWidgetState draft{.type = "photos"};
  expect(!desktop_setup::validate(draft).empty(), "empty Photos requires a source");
  draft.settings["folder_path"] = std::string(path);
  expect(desktop_setup::validate(draft).empty(), "readable album accepted");
  draft.settings["image_path"] = std::string(path) + "/missing.png";
  expect(!desktop_setup::validate(draft).empty(), "invalid overriding image rejected");
  draft = {.type = "news", .settings = {{"feed_url", std::string("https://")}}};
  expect(!desktop_setup::validate(draft).empty(), "incomplete feed URL rejected");
  draft.settings["feed_url"] = std::string("https://example.test/rss");
  expect(desktop_setup::validate(draft).empty(), "complete feed URL accepted without fetching");
  draft.settings["feed_url"] = std::string(" https://example.test/rss ");
  expect(!desktop_setup::validate(draft).empty(), "URL whitespace rejected consistently with the source loader");
  draft = {.type = "reminders", .settings = {{"items", std::vector<std::string>{" "}}}};
  expect(!desktop_setup::validate(draft).empty(), "blank task rejected");
  draft.settings["items"] = std::vector<std::string>{"Review changes"};
  expect(desktop_setup::validate(draft).empty(), "task list accepted");
  draft = {.type = "contacts", .settings = {{"entries", WidgetSettingStringMap{{"Person", "https://example.test"}}}}};
  expect(!desktop_setup::validate(draft).empty(), "contacts require contact URIs");
  draft.settings["entries"] = WidgetSettingStringMap{{"Person", "mailto:person@example.test"}};
  expect(desktop_setup::validate(draft).empty(), "email contact accepted");
  const std::string token = std::string(path) + "/token";
  std::ofstream(token) << "fixture-token\n";
  draft = {
      .type = "home",
      .settings = {
          {"server_url", std::string("https://home.example.test")},
          {"token_file", token},
          {"entities", std::vector<std::string>{"light.desk"}}
      }
  };
  expect(desktop_setup::validate(draft).empty(), "Home settings accepted without making a request");
  draft.type = "find_my";
  expect(!desktop_setup::validate(draft).empty(), "location cards reject non-location entities");
  draft.settings["entities"] = std::vector<std::string>{"person.fixture"};
  expect(desktop_setup::validate(draft).empty(), "location tracker accepted");
  std::ofstream(token) << " \n";
  expect(!desktop_setup::validate(draft).empty(), "blank credentials rejected");
  std::ofstream(token) << "first\nsecond";
  expect(!desktop_setup::validate(draft).empty(), "multiline credentials rejected");
  draft = {.type = "stack", .settings = {{"members", std::vector<std::string>{"clock", "clock"}}}};
  expect(!desktop_setup::validate(draft).empty(), "duplicate stack cards rejected");
  draft.settings["members"] = std::vector<std::string>{"clock", "news"};
  expect(desktop_setup::validate(draft).empty(), "two different cards accepted");
  std::vector<DesktopWidgetState> widgets{
      {.id = "clock", .type = "clock"},
      {.id = "news", .type = "news"},
      {.id = "label", .type = "label"},
      {.id = "first",
       .type = "stack",
       .settings = {{"members", std::vector<std::string>{"news", "missing", "news", "second", "label", "clock"}}}},
      {.id = "second", .type = "stack", .settings = {{"members", std::vector<std::string>{"first", "clock"}}}},
  };
  const auto groups = desktop_stacks::resolve(widgets);
  expect(
      groups.at("first") == std::vector<std::string>{"news", "clock"},
      "membership retains order, ignores duplicate/missing/non-card/nested references"
  );
  expect(groups.at("second").empty(), "one stack owns each card, cycles cannot recurse");
  expect(
      desktop_stacks::contains(groups, "news") && !desktop_stacks::contains(groups, "label"),
      "only stack children are hidden individually"
  );
  const auto cards = desktop_stacks::cards(widgets, "first");
  expect(cards.size() == 2 && cards[0].id == "news", "factory receives ordered member definitions");
  widgets.erase(widgets.begin() + 3);
  expect(!desktop_stacks::contains(desktop_stacks::resolve(widgets), "news"), "removing a stack releases its cards");
  widgets = {
      {.id = "clock", .type = "clock", .cx = 200, .cy = 200},
      {.id = "news",
       .type = "news",
       .cx = 600,
       .cy = 200,
       .settings = {{"card_size", std::string("small")}, {"feed_url", std::string("https://example.test/rss")}}},
      {.id = "tips", .type = "tips"},
      {.id = "plugin", .type = "example.plugin"}
  };
  const auto original = widgets;
  expect(desktop_stacks::join(widgets, "clock", "news", "stack") == "stack", "drop creates a stack at the target");
  expect(widgets[0] == original[0] && widgets[1] == original[1], "stack creation preserves member definitions");
  expect(
      widgets.back().cx == 600 && desktop_stacks::members(widgets.back()) == std::vector<std::string>{"news", "clock"},
      "target supplies placement and first page"
  );
  expect(
      !desktop_stacks::canJoin(widgets, "clock", "tips") && !desktop_stacks::canJoin(widgets, "stack", "tips"),
      "owned and nested sources cannot join"
  );
  expect(!desktop_stacks::canJoin(widgets, "plugin", "stack"), "unsupported widget cannot join");
  expect(desktop_stacks::join(widgets, "tips", "stack", "unused") == "stack", "drop appends to an existing stack");
  expect(desktop_stacks::reorder(widgets, "stack", "tips", 0), "member can move to the front");
  expect(
      desktop_stacks::members(widgets.back()) == std::vector<std::string>{"tips", "news", "clock"},
      "reorder preserves other relative positions"
  );
  expect(
      desktop_stacks::detach(widgets, "stack", "news", "SECOND", 900, 500), "card can be pulled onto another output"
  );
  expect(
      widgets[1].cx == 900 && widgets[1].outputName == "SECOND" && widgets[1].settings == original[1].settings,
      "detach moves the card without losing its source settings"
  );
  expect(desktop_stacks::detach(widgets, "stack", "tips", "FIRST", 100, 300), "second detach dissolves the stack");
  expect(
      std::ranges::none_of(widgets, [](const auto& state) { return state.type == "stack"; }) && widgets[0].cx == 600,
      "last member takes the old stack's position"
  );
  expect(!desktop_stacks::detach(widgets, "missing", "clock", "FIRST", 0, 0), "invalid detach leaves widgets intact");
  using Schedule = desktop_stacks::RotationSchedule;
  using namespace std::chrono_literals;
  const auto epoch = Schedule::Clock::time_point{};
  Schedule schedule(10, epoch);
  expect(
      !schedule.due(false, epoch + 9s) && schedule.due(false, epoch + 10s), "rotation observes the configured interval"
  );
  expect(!schedule.due(true, epoch + 40s) && !schedule.due(false, epoch + 49s), "interaction starts a fresh interval");
  expect(schedule.due(false, epoch + 50s), "rotation resumes after interaction");
  expect(
      schedule.due(false, epoch + 500s) && !schedule.due(false, epoch + 500s),
      "late ticks advance once without catching up"
  );
  schedule.reset(epoch + 600s);
  expect(!schedule.due(false, epoch + 609s), "manual navigation restarts rotation timing");
  Schedule minimum(0, epoch);
  expect(
      !minimum.due(false, epoch + 4s) && minimum.due(false, epoch + 5s), "rotation cannot run faster than five seconds"
  );
  std::println("PASS: guided source validation and safe stack membership resolution");
}
