#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

class ClipboardService;

// Date maths in the launcher, as Raycast's calculator does it: "3 days from now", "2 weeks ago",
// "days until 25 dec", "next friday", "today + 10 days", "weeks since 2026-01-01". Return copies
// the answer.
class DateProvider : public LauncherProvider {
public:
  explicit DateProvider(ClipboardService* clipboard) : m_clipboard(clipboard) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "date"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Date"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "calendar"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;

  struct Answer {
    // The resulting day, when the question asks for one.
    std::optional<std::chrono::year_month_day> date;
    // The number of days/weeks between two dates, when the question asks for one.
    std::optional<long> count;
    std::string unit;   // "days" or "weeks" for a count
    std::string text;   // what is copied: the formatted date, or the count with its unit
    std::string detail; // "in 83 days", "12 weeks ago", the weekday
  };

  // Answers `text` relative to `today`; nullopt when it isn't a date question. `prefixed` allows
  // bare dates and "today". Exposed for tests.
  [[nodiscard]] static std::optional<Answer>
  answer(std::string_view text, std::chrono::year_month_day today, bool prefixed);
  // "25 dec", "dec 25 2026", "2026-12-25", "25/12", "tomorrow", "next friday" → a day.
  [[nodiscard]] static std::optional<std::chrono::year_month_day>
  parseDate(std::string_view text, std::chrono::year_month_day today);
  // "Friday, 25 December 2026".
  [[nodiscard]] static std::string formatDate(std::chrono::year_month_day date);

private:
  ClipboardService* m_clipboard = nullptr;
};
