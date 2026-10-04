#include "launcher/date_provider.h"

#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>

namespace {

  using namespace std::chrono;

  constexpr double kScore = 9000.0;

  constexpr std::array<std::string_view, 12> kMonths = {"january",   "february", "march",    "april",
                                                        "may",       "june",     "july",     "august",
                                                        "september", "october",  "november", "december"};
  constexpr std::array<std::string_view, 7> kWeekdays = {"sunday",   "monday", "tuesday", "wednesday",
                                                         "thursday", "friday", "saturday"};

  [[nodiscard]] std::optional<unsigned> monthFor(std::string_view word) {
    if (word.size() < 3) {
      return std::nullopt;
    }
    for (unsigned i = 0; i < kMonths.size(); ++i) {
      if (kMonths[i].starts_with(word) || word.starts_with(kMonths[i])) {
        return i + 1;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<unsigned> weekdayFor(std::string_view word) {
    if (word.size() < 3) {
      return std::nullopt;
    }
    for (unsigned i = 0; i < kWeekdays.size(); ++i) {
      if (kWeekdays[i].starts_with(word)) {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<long> number(std::string_view word) {
    if (word.empty()) {
      return std::nullopt;
    }
    long value = 0;
    const auto [ptr, ec] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (ec != std::errc{} || ptr != word.data() + word.size()) {
      return std::nullopt;
    }
    return value;
  }

  // "25", "25th", "1st", "2nd", "3rd".
  [[nodiscard]] std::optional<unsigned> dayNumber(std::string_view word) {
    for (const std::string_view suffix : {"st", "nd", "rd", "th"}) {
      if (word.ends_with(suffix) && word.size() > suffix.size()) {
        word.remove_suffix(suffix.size());
        break;
      }
    }
    const auto value = number(word);
    if (!value.has_value() || *value < 1 || *value > 31) {
      return std::nullopt;
    }
    return static_cast<unsigned>(*value);
  }

  [[nodiscard]] sys_days toDays(year_month_day date) { return sys_days{date}; }

  [[nodiscard]] year_month_day addMonths(year_month_day date, long count) {
    year_month_day moved = date + months{count};
    if (!moved.ok()) {
      moved = moved.year() / moved.month() / last;
    }
    return moved;
  }

  [[nodiscard]] std::optional<year_month_day> shift(year_month_day from, long count, std::string_view unit, bool back) {
    const long signedCount = back ? -count : count;
    if (unit.starts_with("day")) {
      return year_month_day{toDays(from) + days{signedCount}};
    }
    if (unit.starts_with("week")) {
      return year_month_day{toDays(from) + days{signedCount * 7}};
    }
    if (unit.starts_with("month")) {
      return addMonths(from, signedCount);
    }
    if (unit.starts_with("year")) {
      return addMonths(from, signedCount * 12);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isUnit(std::string_view word) {
    return word.starts_with("day") || word.starts_with("week") || word.starts_with("month") || word.starts_with("year");
  }

  [[nodiscard]] std::string relative(year_month_day date, year_month_day today) {
    const long delta = (toDays(date) - toDays(today)).count();
    if (delta == 0) {
      return i18n::tr("launcher.date.today");
    }
    if (delta == 1) {
      return i18n::tr("launcher.date.tomorrow");
    }
    if (delta == -1) {
      return i18n::tr("launcher.date.yesterday");
    }
    const long magnitude = delta < 0 ? -delta : delta;
    const std::string amount = magnitude % 7 == 0 ? i18n::trp("launcher.date.units.week", magnitude / 7)
                                                  : i18n::trp("launcher.date.units.day", magnitude);
    return i18n::tr(delta > 0 ? "launcher.date.in" : "launcher.date.ago", "amount", amount);
  }

} // namespace

std::string DateProvider::displayName() const { return i18n::tr("launcher.providers.date.title"); }

std::string DateProvider::formatDate(year_month_day date) {
  std::tm tm{};
  tm.tm_year = static_cast<int>(date.year()) - 1900;
  tm.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
  tm.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
  tm.tm_wday = static_cast<int>(weekday{toDays(date)}.c_encoding());
  char weekdayName[64];
  char monthName[64];
  const std::size_t weekdayLength = std::strftime(weekdayName, sizeof(weekdayName), "%A", &tm);
  const std::size_t monthLength = std::strftime(monthName, sizeof(monthName), "%B", &tm);
  return std::format(
      "{}, {} {} {}", std::string_view(weekdayName, weekdayLength), static_cast<unsigned>(date.day()),
      std::string_view(monthName, monthLength), static_cast<int>(date.year())
  );
}

std::optional<year_month_day> DateProvider::parseDate(std::string_view text, year_month_day today) {
  const std::string lower = StringUtils::toLower(StringUtils::trim(text));
  if (lower.empty()) {
    return std::nullopt;
  }
  if (lower == "today" || lower == "now") {
    return today;
  }
  if (lower == "tomorrow") {
    return year_month_day{toDays(today) + days{1}};
  }
  if (lower == "yesterday") {
    return year_month_day{toDays(today) - days{1}};
  }

  // ISO: 2026-12-25.
  if (lower.size() == 10 && lower[4] == '-' && lower[7] == '-') {
    const auto y = number(std::string_view(lower).substr(0, 4));
    const auto m = number(std::string_view(lower).substr(5, 2));
    const auto d = number(std::string_view(lower).substr(8, 2));
    if (y && m && d) {
      const year_month_day date{
          year{static_cast<int>(*y)}, month{static_cast<unsigned>(*m)}, day{static_cast<unsigned>(*d)}
      };
      return date.ok() ? std::optional(date) : std::nullopt;
    }
    return std::nullopt;
  }

  // Numeric day/month[/year]: 25/12, 25.12.2026 (day first, as outside the US).
  for (const char separator : {'/', '.'}) {
    if (lower.find(separator) == std::string::npos) {
      continue;
    }
    const auto parts = StringUtils::split(lower, separator);
    if (parts.size() < 2 || parts.size() > 3) {
      return std::nullopt;
    }
    const auto d = number(parts[0]);
    const auto m = number(parts[1]);
    if (!d || !m) {
      return std::nullopt;
    }
    int y = static_cast<int>(today.year());
    if (parts.size() == 3) {
      const auto parsed = number(parts[2]);
      if (!parsed) {
        return std::nullopt;
      }
      y = static_cast<int>(*parsed < 100 ? 2000 + *parsed : *parsed);
    }
    const year_month_day date{year{y}, month{static_cast<unsigned>(*m)}, day{static_cast<unsigned>(*d)}};
    return date.ok() ? std::optional(date) : std::nullopt;
  }

  const auto words = StringUtils::split(lower, ' ');
  std::vector<std::string_view> tokens;
  for (const auto word : words) {
    if (!word.empty() && word != "of" && word != "the") {
      tokens.push_back(word);
    }
  }
  if (tokens.empty()) {
    return std::nullopt;
  }

  // "next friday", "last monday", "friday" (the coming one), "this friday".
  {
    std::size_t index = 0;
    int direction = 1;
    bool explicitDirection = false;
    if (tokens[0] == "next" || tokens[0] == "this") {
      index = 1;
      explicitDirection = true;
    } else if (tokens[0] == "last") {
      index = 1;
      direction = -1;
      explicitDirection = true;
    }
    if (index < tokens.size() && tokens.size() == index + 1) {
      if (const auto target = weekdayFor(tokens[index])) {
        const unsigned current = weekday{toDays(today)}.c_encoding();
        long delta = 0;
        if (direction > 0) {
          delta = (static_cast<long>(*target) - static_cast<long>(current) + 7) % 7;
          if (delta == 0) {
            delta = 7;
          }
          if (tokens[0] == "next" && explicitDirection && delta < 7) {
            // "next friday" on a Wednesday is this week's Friday in everyday speech; keep it simple.
          }
        } else {
          delta = -((static_cast<long>(current) - static_cast<long>(*target) + 7) % 7);
          if (delta == 0) {
            delta = -7;
          }
        }
        return year_month_day{toDays(today) + days{delta}};
      }
      if (explicitDirection) {
        if (tokens[index] == "week") {
          return year_month_day{toDays(today) + days{7 * direction}};
        }
        if (tokens[index] == "month") {
          return addMonths(today, direction);
        }
        if (tokens[index] == "year") {
          return addMonths(today, 12 * direction);
        }
      }
    }
  }

  // "25 dec", "25 dec 2026", "dec 25", "december 25th 2026", "25th of december".
  std::optional<unsigned> dayValue;
  std::optional<unsigned> monthValue;
  std::optional<int> yearValue;
  for (const auto token : tokens) {
    if (!monthValue && monthFor(token)) {
      monthValue = monthFor(token);
    } else if (!dayValue && dayNumber(token)) {
      dayValue = dayNumber(token);
    } else if (!yearValue && number(token) && *number(token) >= 1900 && *number(token) <= 2200) {
      yearValue = static_cast<int>(*number(token));
    } else {
      return std::nullopt;
    }
  }
  if (!monthValue) {
    return std::nullopt;
  }
  if (!dayValue) {
    dayValue = 1;
  }
  int y = yearValue.value_or(static_cast<int>(today.year()));
  year_month_day date{year{y}, month{*monthValue}, day{*dayValue}};
  if (!date.ok()) {
    return std::nullopt;
  }
  // A bare "25 dec" means the next one.
  if (!yearValue && toDays(date) < toDays(today)) {
    date = year{y + 1} / date.month() / date.day();
  }
  return date;
}

std::optional<DateProvider::Answer> DateProvider::answer(std::string_view text, year_month_day today, bool prefixed) {
  const std::string lower = StringUtils::toLower(StringUtils::trim(text));
  if (lower.empty()) {
    return std::nullopt;
  }
  const auto words = StringUtils::split(lower, ' ');
  std::vector<std::string_view> tokens;
  for (const auto word : words) {
    if (!word.empty()) {
      tokens.push_back(word);
    }
  }
  if (tokens.empty()) {
    return std::nullopt;
  }

  const auto dateAnswer = [&](year_month_day date) {
    Answer out;
    out.date = date;
    out.text = formatDate(date);
    out.detail = relative(date, today);
    return out;
  };
  const auto countAnswer = [&](year_month_day from, year_month_day to, std::string_view unit) {
    const long dayCount = (toDays(to) - toDays(from)).count();
    Answer out;
    out.count = unit.starts_with("week") ? dayCount / 7 : dayCount;
    out.unit = unit.starts_with("week") ? "weeks" : "days";
    out.text = i18n::trp(unit.starts_with("week") ? "launcher.date.units.week" : "launcher.date.units.day", *out.count);
    out.detail = formatDate(to);
    return out;
  };

  // "3 days from now", "2 weeks ago", "in 10 days".
  {
    std::size_t index = 0;
    if (tokens[0] == "in" && tokens.size() >= 3) {
      index = 1;
    }
    if (tokens.size() >= index + 2) {
      const auto count = number(tokens[index]);
      if (count && *count >= 0 && isUnit(tokens[index + 1])) {
        const std::vector<std::string_view> rest(tokens.begin() + static_cast<long>(index) + 2, tokens.end());
        bool back = false;
        bool valid = false;
        if (rest.empty() && index == 1) {
          valid = true; // "in 10 days"
        } else if (rest.size() == 1 && rest[0] == "ago") {
          back = true;
          valid = true;
        } else if (rest.size() == 2 && rest[0] == "from" && (rest[1] == "now" || rest[1] == "today")) {
          valid = true;
        } else if (rest.size() >= 2 && rest[0] == "from") {
          std::string base;
          for (std::size_t i = 1; i < rest.size(); ++i) {
            base += (i > 1 ? " " : "") + std::string(rest[i]);
          }
          if (const auto from = parseDate(base, today)) {
            if (const auto date = shift(*from, *count, tokens[index + 1], false)) {
              return dateAnswer(*date);
            }
          }
          return std::nullopt;
        } else if (rest.size() >= 2 && rest[0] == "before") {
          std::string base;
          for (std::size_t i = 1; i < rest.size(); ++i) {
            base += (i > 1 ? " " : "") + std::string(rest[i]);
          }
          if (const auto from = parseDate(base, today)) {
            if (const auto date = shift(*from, *count, tokens[index + 1], true)) {
              return dateAnswer(*date);
            }
          }
          return std::nullopt;
        }
        if (valid) {
          if (const auto date = shift(today, *count, tokens[index + 1], back)) {
            return dateAnswer(*date);
          }
        }
      }
    }
  }

  // "days until 25 dec", "weeks since 2026-01-01", "how many days until christmas eve" (any date).
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    const bool unit = tokens[i] == "days" || tokens[i] == "weeks";
    const std::string_view link = tokens[i + 1];
    if (!unit || (link != "until" && link != "till" && link != "to" && link != "since" && link != "from")) {
      continue;
    }
    std::string target;
    for (std::size_t j = i + 2; j < tokens.size(); ++j) {
      target += (j > i + 2 ? " " : "") + std::string(tokens[j]);
    }
    const auto date = parseDate(target, today);
    if (!date.has_value()) {
      return std::nullopt;
    }
    const bool since = link == "since" || link == "from";
    return since ? countAnswer(*date, today, tokens[i]) : countAnswer(today, *date, tokens[i]);
  }

  // "today + 10 days", "25 dec - 2 weeks", "tomorrow plus 3 days".
  for (std::size_t i = 1; i + 2 < tokens.size(); ++i) {
    const std::string_view op = tokens[i];
    const bool plus = op == "+" || op == "plus";
    const bool minus = op == "-" || op == "minus";
    if (!plus && !minus) {
      continue;
    }
    const auto count = number(tokens[i + 1]);
    if (!count || i + 2 >= tokens.size() || !isUnit(tokens[i + 2]) || i + 3 != tokens.size()) {
      return std::nullopt;
    }
    std::string base;
    for (std::size_t j = 0; j < i; ++j) {
      base += (j > 0 ? " " : "") + std::string(tokens[j]);
    }
    const auto from = parseDate(base, today);
    if (!from.has_value()) {
      return std::nullopt;
    }
    if (const auto date = shift(*from, *count, tokens[i + 2], minus)) {
      return dateAnswer(*date);
    }
    return std::nullopt;
  }

  // "next friday", "last month", and after the prefix any date at all ("25 dec", "today").
  const bool relativeWord = tokens[0] == "next" || tokens[0] == "last" || tokens[0] == "this";
  if (relativeWord || prefixed || (tokens.size() == 1 && weekdayFor(tokens[0]) && tokens[0].size() >= 4)) {
    if (const auto date = parseDate(lower, today)) {
      if (!prefixed && !relativeWord && *date == today) {
        return std::nullopt;
      }
      return dateAnswer(*date);
    }
  }
  return std::nullopt;
}

std::vector<LauncherResult> DateProvider::query(std::string_view text) const {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const year_month_day today{
      year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)},
      day{static_cast<unsigned>(local.tm_mday)}
  };
  const auto result = answer(text, today, false);
  if (!result.has_value()) {
    return {};
  }
  LauncherResult row;
  row.id = "date";
  row.title = result->text;
  row.subtitle = result->detail;
  row.glyphName = "calendar";
  row.kind = i18n::tr("launcher.kinds.date");
  row.score = kScore;
  return {std::move(row)};
}

std::vector<LauncherResult> DateProvider::queryPrefixed(std::string_view text) const {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const year_month_day today{
      year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)},
      day{static_cast<unsigned>(local.tm_mday)}
  };
  const auto result = answer(StringUtils::isBlank(text) ? std::string_view("today") : text, today, true);
  if (!result.has_value()) {
    LauncherResult hint;
    hint.id = "hint";
    hint.title = i18n::tr("launcher.date.hint");
    hint.subtitle = i18n::tr("launcher.date.hint-subtitle");
    hint.glyphName = "calendar";
    hint.kind = i18n::tr("launcher.kinds.date");
    return {std::move(hint)};
  }
  LauncherResult row;
  row.id = "date";
  row.title = result->text;
  row.subtitle = result->detail;
  row.glyphName = "calendar";
  row.kind = i18n::tr("launcher.kinds.date");
  row.score = kScore;
  return {std::move(row)};
}

bool DateProvider::activate(const LauncherResult& result) {
  if (result.id != "date" || m_clipboard == nullptr) {
    return false;
  }
  return m_clipboard->copyText(result.title);
}

std::string DateProvider::primaryActionLabel(const LauncherResult& result) const {
  return i18n::tr(result.id == "date" ? "launcher.actions.copy-date" : "launcher.actions.open");
}
