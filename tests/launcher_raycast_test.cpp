// Covers the launcher's Raycast features that work without a running shell: quicklink matching,
// URL templates, snippet placeholders, script command metadata, time zone parsing, and the alias and
// snippet stores.

#include "launcher/alias_store.h"
#include "launcher/launcher_util.h"
#include "launcher/quicklink_provider.h"
#include "launcher/script_provider.h"
#include "launcher/snippet_provider.h"
#include "launcher/snippet_store.h"
#include "launcher/time_provider.h"
#include "tests/test_check.h"

#include <chrono>
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
  }

  void testSnippets() {
    using namespace std::chrono;
    // 2026-10-02 21:05 UTC, rendered in local time by the provider; compare against the same.
    const auto now = sys_days{2026y / October / 2} + 21h + 5min;
    const std::string expanded = SnippetProvider::expand("Hi {clipboard}, see you {date}", now, "Sam");
    TEST_CHECK(expanded.starts_with("Hi Sam, see you 2026-10-0"));
    TEST_CHECK(SnippetProvider::expand("no placeholders", now, "x") == "no placeholders");

    const fs::path path = fs::temp_directory_path() / ("noctalia-snippets-" + std::to_string(::getpid()) + ".json");
    fs::remove(path);
    {
      SnippetStore store(path.string());
      const std::string first = store.add("Address", "1 Infinite Loop");
      const std::string second = store.add("Sign-off", "Thanks,\nHarvey");
      TEST_CHECK(first != second);
      TEST_CHECK(store.remove(first));
      TEST_CHECK(!store.remove(first));
    }
    SnippetStore reloaded(path.string());
    TEST_CHECK(reloaded.snippets().size() == 1);
    TEST_CHECK(reloaded.snippets()[0].text == "Thanks,\nHarvey");
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
  testSnippets();
  testScripts();
  testTime();
  testAliases();
  return 0;
}
