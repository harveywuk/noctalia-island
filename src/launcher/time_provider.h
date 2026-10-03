#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>
#include <optional>
#include <string>

class ClipboardService;

// Time zones in the launcher: "time in tokyo", "london time", "3pm in new york",
// "9:30 pst to cet". The answer is copied (and pasted) on Return.
class TimeProvider : public LauncherProvider {
public:
  explicit TimeProvider(ClipboardService* clipboard) : m_clipboard(clipboard) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "time"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Time"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "clock"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;

  struct Conversion {
    // Time of day typed by the user, in minutes after midnight; nullopt means "now".
    std::optional<int> minutes;
    // Empty means the local zone.
    std::string fromZone;
    std::string toZone;
    std::string toPlace;
  };

  // Parses a query into a conversion; exposed for tests.
  [[nodiscard]] static std::optional<Conversion> parse(std::string_view text, bool prefixed);
  // Resolves a place ("tokyo", "new york", "pst", "utc") to an IANA zone name.
  [[nodiscard]] static std::optional<std::string> zoneFor(std::string_view place);
  // Parses "3pm", "15:30", "9:05am", "noon", "midnight" into minutes after midnight.
  [[nodiscard]] static std::optional<int> parseTimeOfDay(std::string_view text);
  [[nodiscard]] static std::vector<LauncherResult>
  resultsFor(const Conversion& conversion, std::chrono::system_clock::time_point now);

private:
  ClipboardService* m_clipboard = nullptr;
};
