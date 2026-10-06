#include "shell/desktop/widgets/desktop_remote_source.h"

#include <cstdlib>
#include <print>

namespace {
  void expect(bool condition, const char* reason) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", reason);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using namespace desktop_sources;
  expect(validWebUrl("https://example.test/feed?a=1"), "HTTPS source accepted");
  for (const char* url :
       {"file:///etc/passwd", "javascript:alert(1)", "https://", "http:///empty", "https://a\nHeader: x",
        "https://a b"})
    expect(!validWebUrl(url), "invalid or non-web links rejected");
  const auto rss =
      R"(<rss><channel><item><title>One &amp; Two</title><link>https://example.test/story</link><pubDate>Monday</pubDate><enclosure url="https://example.test/episode.ogg"/></item><item><title>Unsafe</title><link>file:///etc/passwd</link></item></channel></rss>)";
  const auto news = parseFeed(rss, false), podcast = parseFeed(rss, true);
  expect(news.size() == 1 && news[0].title == "One & Two" && news[0].detail == "Monday", "RSS metadata decoded");
  expect(news[0].action.ends_with("/story") && podcast[0].action.ends_with(".ogg"), "podcasts prefer enclosure audio");
  const auto atom = parseFeed(
      R"(<feed xmlns="http://www.w3.org/2005/Atom"><entry><title>Atom</title><link href="https://example.test/atom"/><updated>2026-01-01</updated></entry></feed>)",
      false
  );
  expect(atom.size() == 1 && atom[0].action.ends_with("/atom"), "namespaced Atom entries parsed");
  expect(parseFeed("<rss>", false).empty(), "malformed XML rejected");
  expect(
      parseFeed(
          R"(<!DOCTYPE rss [<!ENTITY secret SYSTEM "file:///etc/passwd">]><rss><channel><item><title>&secret;</title><link>https://example.test</link></item></channel></rss>)",
          false
      )
          .empty(),
      "DTD input rejected without reading entities"
  );
  expect(parseFeed(std::string(2 * 1024 * 1024 + 1, 'x'), false).empty(), "oversize feed rejected");
  const auto states = R"([
    {"entity_id":"light.desk","state":"on","attributes":{"friendly_name":"Desk"}},
    {"entity_id":"switch.offline","state":"unavailable","attributes":{}},
    {"entity_id":"sensor.temperature","state":"20","attributes":{}},
    {"entity_id":"person.fixture","state":"home","attributes":{"friendly_name":"Fixture","latitude":51.5,"longitude":-0.12}},
    {"entity_id":"device_tracker.bad","state":"home","attributes":{"latitude":91,"longitude":0}},
    {"entity_id":"device_tracker.missing","attributes":{"latitude":51,"longitude":0}},
    null, 1, "bad"
  ])";
  const auto home = parseHomeStates(states, {"light.desk", "switch.offline", "sensor.temperature", "unknown"}, false);
  expect(
      home.size() == 3 && home[0].checked && home[0].enabled && home[0].title == "Desk",
      "configured home entities retain order and state"
  );
  expect(!home[1].enabled && !home[2].enabled, "unavailable devices and sensors cannot toggle");
  const auto locations =
      parseHomeStates(states, {"light.desk", "person.fixture", "device_tracker.bad", "device_tracker.missing"}, true);
  expect(
      locations.size() == 3
          && locations[0].enabled
          && locations[0].action.starts_with("https://www.openstreetmap.org/"),
      "only selected location entities expose valid map links"
  );
  expect(
      !locations[1].enabled && !locations[2].enabled, "invalid coordinates or missing state disable location actions"
  );
  for (const char* invalid : {"{", "null", "[]", R"({"Global Quote":5})", R"({"Global Quote":{"01. symbol":5}})"}) {
    expect(parseQuote(invalid).empty(), "malformed or partial quotes rejected");
    expect(parseHomeStates(invalid, {"light.desk"}, false).empty(), "malformed states rejected");
  }
  const auto quote = parseQuote(
      R"({"Global Quote":{"01. symbol":"TEST","05. price":"12.50","10. change percent":"1.2%","07. latest trading day":"2026-01-01"}})"
  );
  expect(
      quote.size() == 1 && quote[0].title == "TEST  12.50" && quote[0].detail == "1.2% · 2026-01-01",
      "quote includes provider trading date"
  );
  std::println("PASS: RSS, Atom, podcast links, bounded XML, Home controls, locations and stock quote parsing");
}
