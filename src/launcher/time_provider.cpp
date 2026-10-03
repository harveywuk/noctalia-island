#include "launcher/time_provider.h"

#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <array>
#include <format>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

  constexpr double kTimeScore = 9500.0;

  // Common names and abbreviations that are not a zone's own city.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 46> kAliases = {{
      {"utc", "UTC"},
      {"gmt", "Europe/London"},
      {"z", "UTC"},
      {"zulu", "UTC"},
      {"uk", "Europe/London"},
      {"bst", "Europe/London"},
      {"britain", "Europe/London"},
      {"england", "Europe/London"},
      {"ireland", "Europe/Dublin"},
      {"cet", "Europe/Paris"},
      {"cest", "Europe/Paris"},
      {"eet", "Europe/Athens"},
      {"germany", "Europe/Berlin"},
      {"france", "Europe/Paris"},
      {"spain", "Europe/Madrid"},
      {"italy", "Europe/Rome"},
      {"est", "America/New_York"},
      {"edt", "America/New_York"},
      {"et", "America/New_York"},
      {"eastern", "America/New_York"},
      {"nyc", "America/New_York"},
      {"cst", "America/Chicago"},
      {"cdt", "America/Chicago"},
      {"central", "America/Chicago"},
      {"mst", "America/Denver"},
      {"mdt", "America/Denver"},
      {"pst", "America/Los_Angeles"},
      {"pdt", "America/Los_Angeles"},
      {"pt", "America/Los_Angeles"},
      {"pacific", "America/Los_Angeles"},
      {"sf", "America/Los_Angeles"},
      {"san francisco", "America/Los_Angeles"},
      {"seattle", "America/Los_Angeles"},
      {"cupertino", "America/Los_Angeles"},
      {"boston", "America/New_York"},
      {"washington", "America/New_York"},
      {"ist", "Asia/Kolkata"},
      {"india", "Asia/Kolkata"},
      {"delhi", "Asia/Kolkata"},
      {"mumbai", "Asia/Kolkata"},
      {"jst", "Asia/Tokyo"},
      {"japan", "Asia/Tokyo"},
      {"beijing", "Asia/Shanghai"},
      {"china", "Asia/Shanghai"},
      {"aest", "Australia/Sydney"},
      {"korea", "Asia/Seoul"},
  }};

  // Lower-cased city ("new york", "los angeles") and full name ("america/new_york") -> zone name.
  const std::unordered_map<std::string, std::string>& zoneIndex() {
    static std::once_flag once;
    static std::unordered_map<std::string, std::string> index;
    std::call_once(once, [] {
      try {
        for (const auto& zone : std::chrono::get_tzdb().zones) {
          const std::string name(zone.name());
          if (!name.contains('/') || name.starts_with("Etc/")) {
            continue;
          }
          std::string city = name.substr(name.rfind('/') + 1);
          std::ranges::replace(city, '_', ' ');
          index.emplace(StringUtils::toLower(city), name);
          index.emplace(StringUtils::toLower(name), name);
        }
      } catch (const std::exception&) {
        // No time zone database: only the aliases resolve, and they fail later on lookup.
      }
    });
    return index;
  }

  [[nodiscard]] std::string placeName(std::string_view zone) {
    if (zone == "UTC") {
      return "UTC";
    }
    std::string city(zone.substr(zone.rfind('/') + 1));
    std::ranges::replace(city, '_', ' ');
    return city;
  }

  [[nodiscard]] std::string offsetLabel(std::chrono::seconds offset) {
    const auto total = offset.count();
    const char sign = total < 0 ? '-' : '+';
    const auto absolute = total < 0 ? -total : total;
    const auto hours = absolute / 3600;
    const auto minutes = (absolute % 3600) / 60;
    if (total == 0) {
      return "UTC";
    }
    return minutes == 0 ? std::format("UTC{}{}", sign, hours) : std::format("UTC{}{}:{:02}", sign, hours, minutes);
  }

  [[nodiscard]] std::string stripTimeWords(std::string_view text) {
    std::string out(text);
    for (const std::string_view word :
         {"what time is it in ", "what's the time in ", "time in ", "time at ", "now in "}) {
      if (out.starts_with(word)) {
        return StringUtils::trim(std::string_view(out).substr(word.size()));
      }
    }
    if (out.ends_with(" time")) {
      return StringUtils::trim(std::string_view(out).substr(0, out.size() - 5));
    }
    if (out.starts_with("time ")) {
      return StringUtils::trim(std::string_view(out).substr(5));
    }
    return {};
  }

} // namespace

std::string TimeProvider::displayName() const { return i18n::tr("launcher.providers.time.title"); }

std::optional<std::string> TimeProvider::zoneFor(std::string_view place) {
  const std::string key = StringUtils::toLower(StringUtils::trim(place));
  if (key.empty()) {
    return std::nullopt;
  }
  for (const auto& [alias, zone] : kAliases) {
    if (alias == key) {
      return std::string(zone);
    }
  }
  const auto& index = zoneIndex();
  if (const auto it = index.find(key); it != index.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<int> TimeProvider::parseTimeOfDay(std::string_view text) {
  std::string value = StringUtils::toLower(StringUtils::trim(text));
  if (value == "noon") {
    return 12 * 60;
  }
  if (value == "midnight") {
    return 0;
  }
  bool pm = false;
  bool am = false;
  if (value.ends_with("pm") || value.ends_with("am")) {
    pm = value.ends_with("pm");
    am = !pm;
    value = StringUtils::trim(std::string_view(value).substr(0, value.size() - 2));
  }
  if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
    return std::nullopt;
  }
  int hours = 0;
  int minutes = 0;
  std::size_t pos = 0;
  while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
    hours = hours * 10 + (value[pos] - '0');
    ++pos;
  }
  if (pos < value.size()) {
    if (value[pos] != ':' && value[pos] != '.') {
      return std::nullopt;
    }
    ++pos;
    const std::size_t start = pos;
    while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
      minutes = minutes * 10 + (value[pos] - '0');
      ++pos;
    }
    if (pos != value.size() || pos - start != 2) {
      return std::nullopt;
    }
  } else if (!am && !pm) {
    // A bare number ("3 in tokyo") is not a time.
    return std::nullopt;
  }
  if (minutes > 59 || hours > 23 || ((am || pm) && (hours == 0 || hours > 12))) {
    return std::nullopt;
  }
  if (pm && hours != 12) {
    hours += 12;
  } else if (am && hours == 12) {
    hours = 0;
  }
  return hours * 60 + minutes;
}

std::optional<TimeProvider::Conversion> TimeProvider::parse(std::string_view text, bool prefixed) {
  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  if (query.empty()) {
    return std::nullopt;
  }

  // "time in tokyo", "london time", or (after the prefix) just "tokyo".
  std::string place = stripTimeWords(query);
  if (place.empty() && prefixed) {
    place = query;
  }
  if (!place.empty()) {
    if (const auto zone = zoneFor(place)) {
      return Conversion{.minutes = std::nullopt, .fromZone = {}, .toZone = *zone, .toPlace = placeName(*zone)};
    }
  }

  // "<time> [<from>] in|to <place>".
  for (const std::string_view separator : {" in ", " to "}) {
    const auto split = query.rfind(separator);
    if (split == std::string::npos) {
      continue;
    }
    const std::string left = StringUtils::trim(std::string_view(query).substr(0, split));
    const std::string right = StringUtils::trim(std::string_view(query).substr(split + separator.size()));
    const auto toZone = zoneFor(right);
    if (!toZone.has_value()) {
      continue;
    }
    // The time is the first word, or the first two when written "3 pm".
    const auto firstSpace = left.find(' ');
    std::optional<int> minutes = parseTimeOfDay(left.substr(0, firstSpace));
    std::string rest = firstSpace == std::string::npos ? std::string() : StringUtils::trim(left.substr(firstSpace));
    if (!minutes.has_value() && firstSpace != std::string::npos) {
      const auto secondSpace = left.find(' ', firstSpace + 1);
      minutes = parseTimeOfDay(left.substr(0, secondSpace));
      rest = secondSpace == std::string::npos ? std::string() : StringUtils::trim(left.substr(secondSpace));
    }
    if (!minutes.has_value()) {
      if (left == "now") {
        return Conversion{.minutes = std::nullopt, .fromZone = {}, .toZone = *toZone, .toPlace = placeName(*toZone)};
      }
      continue;
    }
    std::string fromZone;
    if (!rest.empty()) {
      const auto zone = zoneFor(rest);
      if (!zone.has_value()) {
        continue;
      }
      fromZone = *zone;
    }
    return Conversion{.minutes = minutes, .fromZone = fromZone, .toZone = *toZone, .toPlace = placeName(*toZone)};
  }
  return std::nullopt;
}

std::vector<LauncherResult>
TimeProvider::resultsFor(const Conversion& conversion, std::chrono::system_clock::time_point now) {
  using namespace std::chrono;
  try {
    const time_zone* target = locate_zone(conversion.toZone);
    const time_zone* source = conversion.fromZone.empty() ? current_zone() : locate_zone(conversion.fromZone);

    sys_seconds instant = floor<seconds>(now);
    if (conversion.minutes.has_value()) {
      // That time of day, today, in the source zone.
      const auto sourceToday = floor<days>(zoned_time{source, instant}.get_local_time());
      const local_seconds sourceLocal = sourceToday + minutes(*conversion.minutes);
      instant = source->to_sys(sourceLocal, choose::earliest);
    }

    const zoned_time targetTime{target, instant};
    const auto targetLocal = targetTime.get_local_time();
    const auto info = targetTime.get_info();
    const auto localDay = floor<days>(zoned_time{current_zone(), instant}.get_local_time());
    const auto targetDay = floor<days>(targetLocal);

    LauncherResult result;
    result.id = "time";
    result.title = std::format("{:%H:%M}", targetLocal);
    const year_month_day date{targetDay};
    std::string day = std::format("{:%a} {} {:%b}", targetLocal, static_cast<unsigned>(date.day()), targetLocal);
    if (targetDay > localDay) {
      day += " · " + i18n::tr("launcher.time.tomorrow");
    } else if (targetDay < localDay) {
      day += " · " + i18n::tr("launcher.time.yesterday");
    }
    std::string subtitle = conversion.toPlace + " · " + StringUtils::trim(day) + " · " + offsetLabel(info.offset);
    if (conversion.minutes.has_value()) {
      const zoned_time sourceTime{source, instant};
      const std::string sourcePlace =
          conversion.fromZone.empty() ? i18n::tr("launcher.time.local") : placeName(conversion.fromZone);
      subtitle += " · " + std::format("{:%H:%M}", sourceTime.get_local_time()) + " " + sourcePlace;
    }
    result.subtitle = std::move(subtitle);
    result.glyphName = "clock";
    result.kind = i18n::tr("launcher.kinds.time");
    result.score = kTimeScore;
    return {std::move(result)};
  } catch (const std::exception&) {
    return {};
  }
}

std::vector<LauncherResult> TimeProvider::query(std::string_view text) const {
  const auto conversion = parse(text, false);
  return conversion.has_value() ? resultsFor(*conversion, std::chrono::system_clock::now())
                                : std::vector<LauncherResult>{};
}

std::vector<LauncherResult> TimeProvider::queryPrefixed(std::string_view text) const {
  if (StringUtils::isBlank(text)) {
    return {};
  }
  const auto conversion = parse(text, true);
  return conversion.has_value() ? resultsFor(*conversion, std::chrono::system_clock::now())
                                : std::vector<LauncherResult>{};
}

bool TimeProvider::activate(const LauncherResult& result) {
  return m_clipboard != nullptr && m_clipboard->copyText(result.title);
}

std::string TimeProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.copy-time");
}
