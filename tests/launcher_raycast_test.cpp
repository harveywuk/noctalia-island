// Covers the launcher's Raycast features that work without a running shell: quicklink matching,
// URL templates, snippet placeholders, script command metadata, time zone parsing, form validation,
// and the alias, quicklink and snippet stores.

#include "launcher/alias_store.h"
#include "launcher/clipboard_provider.h"
#include "launcher/date_provider.h"
#include "launcher/launcher_util.h"
#include "launcher/math_provider.h"
#include "launcher/process_provider.h"
#include "launcher/quicklink_provider.h"
#include "launcher/quicklink_store.h"
#include "launcher/script_provider.h"
#include "launcher/snippet_provider.h"
#include "launcher/snippet_store.h"
#include "launcher/time_provider.h"
#include "launcher/timer_provider.h"
#include "launcher/usage_tracker.h"
#include "launcher/window_management_provider.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  void testUrls() {
    TEST_CHECK(launcher_util::urlEncode("a b&c/é") == "a%20b%26c%2F%C3%A9");
    TEST_CHECK(
        launcher_util::fillUrlTemplate("https://example.com/?q={query}&again={query}", "two words")
        == "https://example.com/?q=two%20words&again=two%20words"
    );
    TEST_CHECK(launcher_util::fillUrlTemplate("https://example.com/", "ignored") == "https://example.com/");
    TEST_CHECK(launcher_util::fileUri("/home/me/My File.txt") == "file:///home/me/My%20File.txt");
    TEST_CHECK(launcher_util::formatByteSize(512) == "512 B");
    TEST_CHECK(launcher_util::formatByteSize(1500) == "1.5 KB");
    TEST_CHECK(launcher_util::formatByteSize(34'000'000) == "34 MB");
    TEST_CHECK(launcher_util::wordsMatch("dark mo", "toggle dark mode appearance"));
    TEST_CHECK(!launcher_util::wordsMatch("sig", "toggle night light"));

    // The clipboard image action: {path} gets the export, otherwise the image is piped in.
    TEST_CHECK(ClipboardProvider::imageActionCommand("gimp {path}", "/tmp/a b.png") == "gimp '/tmp/a b.png'");
    TEST_CHECK(
        ClipboardProvider::imageActionCommand("satty -f {stdin}", "/tmp/x.png") == "cat -- '/tmp/x.png' | satty -f -"
    );
    TEST_CHECK(ClipboardProvider::imageActionCommand("gradia", "/tmp/x.png") == "cat -- '/tmp/x.png' | gradia");
  }

  void testQuicklinkForms() {
    TEST_CHECK(QuicklinkProvider::normalizeUrl("github.com/search?q={query}") == "https://github.com/search?q={query}");
    TEST_CHECK(QuicklinkProvider::normalizeUrl(" http://localhost:8080 ") == "http://localhost:8080");
    TEST_CHECK(QuicklinkProvider::normalizeUrl("mailto:me@example.com") == "mailto:me@example.com");
    TEST_CHECK(QuicklinkProvider::normalizeUrl("not a link").empty());
    TEST_CHECK(QuicklinkProvider::normalizeUrl("https://").empty());

    const auto builtins = QuicklinkProvider::builtinQuicklinks();
    TEST_CHECK(QuicklinkProvider::validate(builtins, {}, "example.com", "ex").empty());
    // "gh" belongs to GitHub, unless GitHub itself is being edited.
    TEST_CHECK(!QuicklinkProvider::validate(builtins, {}, "example.com", "GH").empty());
    TEST_CHECK(QuicklinkProvider::validate(builtins, "github", "github.com", "gh").empty());
    TEST_CHECK(!QuicklinkProvider::validate(builtins, {}, "example.com", "two words").empty());
    TEST_CHECK(!QuicklinkProvider::validate(builtins, {}, "", "").empty());

    const fs::path path = fs::temp_directory_path() / ("noctalia-quicklinks-" + std::to_string(::getpid()) + ".json");
    fs::remove(path);
    {
      QuicklinkStore store(path.string());
      const std::string id =
          store.put({.id = {}, .name = "Docs", .url = "https://docs.example.com", .keyword = "d", .glyph = {}});
      TEST_CHECK(!id.empty());
      TEST_CHECK(
          store.put({.id = id, .name = "Docs 2", .url = "https://docs.example.com", .keyword = "d", .glyph = {}}) == id
      );
      // Saving a built-in's id replaces that built-in.
      store.put(
          {.id = "github",
           .name = "GitHub Code",
           .url = "https://github.com/search?type=code&q={query}",
           .keyword = "gh",
           .glyph = {}}
      );
      TEST_CHECK(store.quicklinks().size() == 2);
    }
    QuicklinkStore reloaded(path.string());
    TEST_CHECK(reloaded.quicklinks().size() == 2);
    const auto merged = QuicklinkProvider::effectiveQuicklinks(nullptr, &reloaded);
    TEST_CHECK(merged.size() == builtins.size() + 1);
    const auto github = std::ranges::find(merged, std::string("github"), &LauncherQuicklinkConfig::id);
    TEST_CHECK(github != merged.end() && github->name == "GitHub Code");
    TEST_CHECK(reloaded.remove("github"));
    TEST_CHECK(!reloaded.remove("github"));
    fs::remove(path);
  }

  void testQuicklinks() {
    const std::vector<LauncherQuicklinkConfig> links = {
        {.id = "github", .name = "GitHub", .url = "https://github.com/search?q={query}", .keyword = "gh", .glyph = {}},
        {.id = "news", .name = "Hacker News", .url = "https://news.ycombinator.com", .keyword = {}, .glyph = {}},
    };
    // "<keyword> <query>" searches straight away.
    const auto search = QuicklinkProvider::match(links, "gh noctalia island", false);
    TEST_CHECK(search.size() == 1);
    TEST_CHECK(search[0].id == "link:github");
    TEST_CHECK(search[0].query == std::optional<std::string>("noctalia island"));

    // A name match opens the link.
    const auto named = QuicklinkProvider::match(links, "hacker", false);
    TEST_CHECK(named.size() == 1 && named[0].id == "link:news" && !named[0].query.has_value());

    // Nothing typed lists every link only after the prefix.
    TEST_CHECK(QuicklinkProvider::match(links, "", false).empty());
    TEST_CHECK(QuicklinkProvider::match(links, "", true).size() == 2);

    // With nothing configured, the built-in set is used.
    TEST_CHECK(!QuicklinkProvider::effectiveQuicklinks(nullptr).empty());

    // Fallback commands: the named search quicklinks offer the query, plain links are skipped.
    const auto fallbacks = QuicklinkProvider::fallbacks(links, {"news", "GitHub"}, " island ");
    TEST_CHECK(fallbacks.size() == 1);
    TEST_CHECK(fallbacks[0].id == "link:github" && fallbacks[0].fallback);
    TEST_CHECK(fallbacks[0].query == std::optional<std::string>("island"));
    TEST_CHECK(QuicklinkProvider::fallbacks(links, {"github"}, "").empty());
  }

  void testSnippets() {
    using namespace std::chrono;
    // 2026-10-02 21:05 UTC, rendered in local time by the provider; compare against the same.
    const auto now = sys_days{2026y / October / 2} + 21h + 5min;
    const std::string expanded = SnippetProvider::expand("Hi {clipboard}, see you {date}", now, "Sam");
    TEST_CHECK(expanded.starts_with("Hi Sam, see you 2026-10-0"));
    TEST_CHECK(SnippetProvider::expand("no placeholders", now, "x") == "no placeholders");
    // Raycast's other placeholders: a fresh UUID each time, the caret marker dropped, calendar parts.
    const std::string uuids = SnippetProvider::expand("{uuid} {uuid}", now, "");
    TEST_CHECK(uuids.size() == 73 && uuids[36] == ' ' && uuids.substr(0, 36) != uuids.substr(37));
    TEST_CHECK(uuids[14] == '4' && uuids[8] == '-' && uuids[23] == '-');
    TEST_CHECK(SnippetProvider::expand("Dear {cursor},", now, "") == "Dear ,");
    TEST_CHECK(SnippetProvider::expand("{year}", now, "").starts_with("202"));
    TEST_CHECK(!SnippetProvider::expand("{weekday} {month} {day}", now, "").contains('{'));

    const fs::path path = fs::temp_directory_path() / ("noctalia-snippets-" + std::to_string(::getpid()) + ".json");
    fs::remove(path);
    {
      SnippetStore store(path.string());
      const std::string first = store.add("Address", "1 Infinite Loop");
      const std::string second = store.add("Sign-off", "Thanks,\nHarvey");
      TEST_CHECK(first != second);
      TEST_CHECK(store.update(second, "Sign-off", "Thanks,\nHarvey", "thx"));
      TEST_CHECK(!store.update("missing", "x", "y", "z"));
      TEST_CHECK(store.remove(first));
      TEST_CHECK(!store.remove(first));
    }
    SnippetStore reloaded(path.string());
    TEST_CHECK(reloaded.snippets().size() == 1);
    TEST_CHECK(reloaded.snippets()[0].text == "Thanks,\nHarvey");
    TEST_CHECK(reloaded.snippets()[0].keyword == "thx");
    // A keyword already in use is refused, except by the snippet being edited.
    TEST_CHECK(!SnippetProvider::validate(nullptr, &reloaded, {}, "THX").empty());
    TEST_CHECK(SnippetProvider::validate(nullptr, &reloaded, "saved:" + reloaded.snippets()[0].id, "thx").empty());
    TEST_CHECK(SnippetProvider::validate(nullptr, &reloaded, {}, "new").empty());
    fs::remove(path);
  }

  void testScripts() {
    const std::string raycast = R"(#!/bin/bash
# Required parameters:
# @raycast.schemaVersion 1
# @raycast.title Translate
# @raycast.mode fullOutput
# Optional parameters:
# @raycast.icon 🌐
# @raycast.argument1 { "type": "text", "placeholder": "Text" }
# @raycast.argument2 { "type": "text", "placeholder": "Language", "optional": true }
# @raycast.packageName Utilities
echo "$1"
)";
    const auto script = ScriptProvider::parse("/tmp/translate.sh", raycast);
    TEST_CHECK(script.has_value());
    TEST_CHECK(script->title == "Translate");
    TEST_CHECK(script->mode == ScriptProvider::Mode::FullOutput);
    TEST_CHECK(script->icon == "🌐");
    TEST_CHECK(script->packageName == "Utilities");
    TEST_CHECK(script->arguments.size() == 2);
    TEST_CHECK(script->arguments[0].placeholder == "Text" && !script->arguments[0].optional);
    TEST_CHECK(script->arguments[1].optional);

    const auto native = ScriptProvider::parse("/tmp/x.py", "#!/usr/bin/env python3\n# @noctalia.title Say Hi\n");
    TEST_CHECK(native.has_value() && native->title == "Say Hi" && native->mode == ScriptProvider::Mode::Compact);
    TEST_CHECK(!ScriptProvider::parse("/tmp/plain.sh", "#!/bin/sh\necho hi\n").has_value());

    // The last argument takes the rest of the line; quotes group words.
    TEST_CHECK((ScriptProvider::splitArguments("hello world", 1) == std::vector<std::string>{"hello world"}));
    TEST_CHECK((
        ScriptProvider::splitArguments("\"good morning\" fr ca", 2) == std::vector<std::string>{"good morning", "fr ca"}
    ));
    TEST_CHECK(ScriptProvider::splitArguments("  ", 2).empty());
  }

  void testTime() {
    TEST_CHECK(TimeProvider::parseTimeOfDay("3pm") == std::optional<int>(15 * 60));
    TEST_CHECK(TimeProvider::parseTimeOfDay("12am") == std::optional<int>(0));
    TEST_CHECK(TimeProvider::parseTimeOfDay("9:05am") == std::optional<int>(9 * 60 + 5));
    TEST_CHECK(TimeProvider::parseTimeOfDay("15:30") == std::optional<int>(15 * 60 + 30));
    TEST_CHECK(TimeProvider::parseTimeOfDay("noon") == std::optional<int>(12 * 60));
    TEST_CHECK(!TimeProvider::parseTimeOfDay("15").has_value());
    TEST_CHECK(!TimeProvider::parseTimeOfDay("13pm").has_value());

    TEST_CHECK(TimeProvider::zoneFor("pst") == std::optional<std::string>("America/Los_Angeles"));

    const bool haveTzdb = TimeProvider::zoneFor("tokyo").has_value();
    if (!haveTzdb) {
      // No time zone database in this environment; the parsing rules above still hold.
      return;
    }
    TEST_CHECK(TimeProvider::zoneFor("New York") == std::optional<std::string>("America/New_York"));

    const auto now = TimeProvider::parse("time in tokyo", false);
    TEST_CHECK(now.has_value() && now->toZone == "Asia/Tokyo" && !now->minutes.has_value());
    TEST_CHECK(TimeProvider::parse("london time", false).has_value());
    TEST_CHECK(TimeProvider::parse("tokyo", true).has_value());
    // A bare place is only a time query after the prefix.
    TEST_CHECK(!TimeProvider::parse("tokyo", false).has_value());

    const auto converted = TimeProvider::parse("3pm london in new york", false);
    TEST_CHECK(converted.has_value());
    TEST_CHECK(converted->minutes == std::optional<int>(15 * 60));
    TEST_CHECK(converted->fromZone == "Europe/London" && converted->toZone == "America/New_York");

    // Plain searches and maths are left alone.
    TEST_CHECK(!TimeProvider::parse("firefox", false).has_value());
    TEST_CHECK(!TimeProvider::parse("10 cm in inches", false).has_value());

    // 3pm in London on a summer day is 10am in New York.
    using namespace std::chrono;
    const auto summer = sys_days{2026y / July / 1} + 9h;
    const auto results = TimeProvider::resultsFor(*converted, summer);
    TEST_CHECK(results.size() == 1 && results[0].title == "10:00");
  }

  void testWindowManagement() {
    using window_management::Layout;
    using window_management::Rect;
    // A 1920x1080 monitor at (0,0) with a 40px bar reserved at the top.
    const Rect area{.x = 0, .y = 40, .width = 1920, .height = 1040};
    const Rect window{.x = 100, .y = 100, .width = 800, .height = 600};
    TEST_CHECK((window_management::frameFor(Layout::LeftHalf, area, window) == Rect{0, 40, 960, 1040}));
    TEST_CHECK((window_management::frameFor(Layout::RightHalf, area, window) == Rect{960, 40, 960, 1040}));
    TEST_CHECK((window_management::frameFor(Layout::BottomRightQuarter, area, window) == Rect{960, 560, 960, 520}));
    TEST_CHECK((window_management::frameFor(Layout::CenterThird, area, window) == Rect{640, 40, 640, 1040}));
    TEST_CHECK((window_management::frameFor(Layout::LastTwoThirds, area, window) == Rect{640, 40, 1280, 1040}));
    TEST_CHECK((window_management::frameFor(Layout::Maximize, area, window) == Rect{0, 40, 1920, 1040}));
    // Center keeps the size; the second monitor's offset carries into the frame.
    const Rect second{.x = 1920, .y = 0, .width = 2560, .height = 1440};
    TEST_CHECK((window_management::frameFor(Layout::Center, second, window) == Rect{2800, 420, 800, 600}));
    // Growing stops at the work area, shrinking at a usable minimum.
    const Rect huge{.x = 0, .y = 0, .width = 5000, .height = 5000};
    TEST_CHECK((window_management::frameFor(Layout::MakeLarger, area, huge) == Rect{0, 40, 1920, 1040}));
    const Rect tiny{.x = 0, .y = 0, .width = 50, .height = 50};
    const Rect smaller = window_management::frameFor(Layout::MakeSmaller, area, tiny);
    TEST_CHECK(smaller.width == 200 && smaller.height == 200);
    TEST_CHECK(window_management::frameFor(Layout::AlmostMaximize, area, window).width == 1728);

    // Every command has a layout or a dispatcher, and a unique id.
    const auto commands = WindowManagementProvider::commands();
    for (const auto& command : commands) {
      TEST_CHECK(command.layout.has_value() || !command.dispatcher.empty());
      TEST_CHECK(std::ranges::count(commands, command.id, &WindowManagementProvider::Command::id) == 1);
    }
  }

  void testTimers() {
    using namespace std::chrono;
    auto request = TimerProvider::parse("timer 10m", false);
    TEST_CHECK(request.has_value() && request->duration == minutes(10) && request->label.empty());
    request = TimerProvider::parse("set timer 1h30 tea", false);
    TEST_CHECK(request.has_value() && request->duration == minutes(90) && request->label == "Tea");
    request = TimerProvider::parse("15 min stretch timer", false);
    TEST_CHECK(request.has_value() && request->duration == minutes(15) && request->label == "Stretch");
    request = TimerProvider::parse("25 minutes focus", true);
    TEST_CHECK(request.has_value() && request->duration == minutes(25) && request->label == "Focus");
    request = TimerProvider::parse("2h", true);
    TEST_CHECK(request.has_value() && request->duration == hours(2));
    request = TimerProvider::parse("90", true);
    TEST_CHECK(request.has_value() && request->duration == minutes(90));
    // Without its prefix the word "timer" is required, and plain searches are left alone.
    TEST_CHECK(!TimerProvider::parse("10 min", false).has_value());
    TEST_CHECK(!TimerProvider::parse("firefox", true).has_value());
    TEST_CHECK(!TimerProvider::parse("timer 0m", false).has_value());
    TEST_CHECK(!TimerProvider::parse("timer 48h", false).has_value());
    TEST_CHECK(TimerProvider::formatRemaining(seconds(299)) == "4:59");
    TEST_CHECK(TimerProvider::formatRemaining(seconds(3661)) == "1:01:01");
    TEST_CHECK(TimerProvider::formatRemaining(seconds(7)) == "0:07");
  }

  void testProcesses() {
    const auto stat = ProcessProvider::parseStat(
        "1234 (my (odd) name) S 1 1234 1234 0 -1 4194560 100 0 0 0 250 50 0 0 20 0 1 0 98765 1000000 300 "
        "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0"
    );
    TEST_CHECK(stat.has_value());
    TEST_CHECK(stat->comm == "my (odd) name" && stat->state == 'S');
    TEST_CHECK(stat->utime == 250 && stat->stime == 50 && stat->starttime == 98765);
    TEST_CHECK(!ProcessProvider::parseStat("garbage").has_value());
    // The live scan lists this test's own parent shell, never the test itself.
    const auto processes = ProcessProvider::scan();
    TEST_CHECK(std::ranges::none_of(processes, [](const ProcessProvider::Process& p) { return p.pid == ::getpid(); }));
    for (const auto& process : processes) {
      TEST_CHECK(!process.name.empty() && process.pid > 0);
    }
  }

  void testDates() {
    using namespace std::chrono;
    const year_month_day today = 2026y / October / 3; // a Saturday
    const auto ymd = [](int y, unsigned m, unsigned d) { return year{y} / month{m} / day{d}; };

    auto a = DateProvider::answer("3 days from now", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 10, 6));
    a = DateProvider::answer("2 weeks ago", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 9, 19));
    a = DateProvider::answer("in 1 month", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 11, 3));
    a = DateProvider::answer("days until 25 dec", today, false);
    TEST_CHECK(a.has_value() && a->count == 83 && a->unit == "days");
    a = DateProvider::answer("weeks since 2026-01-03", today, false);
    TEST_CHECK(a.has_value() && a->count == 39 && a->unit == "weeks");
    a = DateProvider::answer("next friday", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 10, 9));
    a = DateProvider::answer("last monday", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 9, 28));
    a = DateProvider::answer("today + 10 days", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 10, 13));
    a = DateProvider::answer("25 dec - 2 weeks", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 12, 11));
    // Month ends clamp: 31 Oct + 1 month is 30 Nov.
    a = DateProvider::answer("1 month from 31 oct", today, false);
    TEST_CHECK(a.has_value() && a->date == ymd(2026, 11, 30));
    // A past day without a year means next year's.
    TEST_CHECK(DateProvider::parseDate("1 jan", today) == std::optional(ymd(2027, 1, 1)));
    TEST_CHECK(DateProvider::parseDate("25/12", today) == std::optional(ymd(2026, 12, 25)));
    TEST_CHECK(DateProvider::parseDate("december 25th 2027", today) == std::optional(ymd(2027, 12, 25)));
    TEST_CHECK(DateProvider::parseDate("friday", today) == std::optional(ymd(2026, 10, 9)));
    // Plain searches, maths and times are not dates.
    TEST_CHECK(!DateProvider::answer("firefox", today, false).has_value());
    TEST_CHECK(!DateProvider::answer("2+2", today, false).has_value());
    TEST_CHECK(!DateProvider::answer("3pm in tokyo", today, false).has_value());
    TEST_CHECK(!DateProvider::answer("today", today, false).has_value());
    TEST_CHECK(DateProvider::answer("today", today, true).has_value());
    TEST_CHECK(DateProvider::formatDate(ymd(2026, 12, 25)) == "Friday, 25 December 2026");
  }

  void testCalculatorHistory() {
    const fs::path path = fs::temp_directory_path() / ("noctalia-calc-" + std::to_string(::getpid()) + ".json");
    fs::remove(path);
    TEST_CHECK(MathProvider::loadHistory(path.string()).empty());
    std::deque<MathProvider::HistoryEntry> history;
    history.push_front({.expression = "2+2", .result = "4"});
    history.push_front({.expression = "10 cm to in", .result = "3.937 in"});
    MathProvider::saveHistory(path.string(), history);
    const auto loaded = MathProvider::loadHistory(path.string());
    TEST_CHECK(loaded.size() == 2 && loaded.front().result == "3.937 in" && loaded.back().expression == "2+2");
    fs::remove(path);
  }

  void testRecent() {
    // The tracker keeps a short cross-provider history for Suggestions, newest first, de-duplicated.
    const fs::path dir = fs::temp_directory_path() / ("noctalia-usage-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    ::setenv("NOCTALIA_STATE_HOME", dir.c_str(), 1);
    {
      UsageTracker tracker;
      tracker.clear();
      tracker.recordRecent("System", "toggle-dark-mode");
      tracker.recordRecent("Applications", "/usr/share/applications/foot.desktop");
      tracker.recordRecent("System", "toggle-dark-mode");
      const auto& recent = tracker.recent();
      TEST_CHECK(recent.size() == 2);
      TEST_CHECK(recent[0].first == "System" && recent[0].second == "toggle-dark-mode");
      TEST_CHECK(recent[1].first == "Applications");
    }
    UsageTracker reloaded;
    TEST_CHECK(reloaded.recent().size() == 2 && reloaded.recent()[0].first == "System");
    reloaded.clear();
    TEST_CHECK(reloaded.recent().empty());
    fs::remove_all(dir);
  }

  void testAliases() {
    TEST_CHECK(AliasStore::normalize("  FF ") == "ff");
    const auto spec = AliasStore::parseSpec("System:toggle-dark-mode");
    TEST_CHECK(spec.has_value() && spec->providerId == "System" && spec->resultId == "toggle-dark-mode");
    const auto app = AliasStore::parseSpec("firefox.desktop");
    TEST_CHECK(app.has_value() && app->providerId == "Applications" && app->resultId == "firefox.desktop");
    const auto path = AliasStore::parseSpec("/usr/share/applications/a:b.desktop");
    TEST_CHECK(path.has_value() && path->providerId == "Applications");

    const fs::path file = fs::temp_directory_path() / ("noctalia-aliases-" + std::to_string(::getpid()) + ".json");
    fs::remove(file);
    {
      AliasStore store(file.string());
      TEST_CHECK(store.set("ff", {.providerId = "Applications", .resultId = "/apps/firefox.desktop"}));
      TEST_CHECK(!store.set("two words", {.providerId = "System", .resultId = "x"}));
      // Giving the same result a new alias replaces the old one.
      TEST_CHECK(store.set("fx", {.providerId = "Applications", .resultId = "/apps/firefox.desktop"}));
      TEST_CHECK(!store.find("ff").has_value());
      TEST_CHECK(store.aliasFor("Applications", "/apps/firefox.desktop") == "fx");
    }
    AliasStore reloaded(file.string());
    TEST_CHECK(reloaded.find("FX").has_value());
    // Config aliases win on a clash.
    reloaded.setConfigAliases({{"fx", "System:toggle-dnd"}});
    TEST_CHECK(reloaded.find("fx")->providerId == "System");
    TEST_CHECK(reloaded.removeFor("Applications", "/apps/firefox.desktop"));
    fs::remove(file);
  }

} // namespace

int main() {
  testUrls();
  testQuicklinks();
  testQuicklinkForms();
  testSnippets();
  testScripts();
  testTime();
  testWindowManagement();
  testRecent();
  testTimers();
  testProcesses();
  testDates();
  testCalculatorHistory();
  testAliases();
  return 0;
}
