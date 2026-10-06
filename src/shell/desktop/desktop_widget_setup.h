#pragma once

#include "config/config_types.h"

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace desktop_setup {
  [[nodiscard]] bool guided(std::string_view type);
  // An empty result means the draft can be saved. Otherwise this is a translation key.
  [[nodiscard]] std::string validate(const DesktopWidgetState& draft);
} // namespace desktop_setup

namespace desktop_stacks {
  inline constexpr std::size_t maxMembers = 8;
  using Membership = std::unordered_map<std::string, std::vector<std::string>>;
  [[nodiscard]] std::vector<std::string> members(const DesktopWidgetState& stack);
  // First owner wins. Missing references, duplicates, non-cards and nested stacks are ignored.
  [[nodiscard]] Membership resolve(const std::vector<DesktopWidgetState>& widgets);
  [[nodiscard]] bool contains(const Membership& membership, const std::string& id);
  [[nodiscard]] std::vector<DesktopWidgetState>
  cards(const std::vector<DesktopWidgetState>& widgets, const std::string& stackId);
  [[nodiscard]] bool
  canJoin(const std::vector<DesktopWidgetState>& widgets, const std::string& source, const std::string& target);
  // Returns the destination stack ID. Member definitions retain their individual settings and placement.
  [[nodiscard]] std::string join(
      std::vector<DesktopWidgetState>& widgets, const std::string& source, const std::string& target,
      const std::string& newId
  );
  bool reorder(
      std::vector<DesktopWidgetState>& widgets, const std::string& stack, const std::string& member, std::size_t to
  );
  bool detach(
      std::vector<DesktopWidgetState>& widgets, const std::string& stack, const std::string& member,
      const std::string& output, float x, float y
  );

  class RotationSchedule {
  public:
    using Clock = std::chrono::steady_clock;
    explicit RotationSchedule(int seconds = 30, Clock::time_point now = Clock::now());
    void reset(Clock::time_point now = Clock::now());
    // Pauses start a fresh interval; a late tick advances once, never catches up in a burst.
    bool due(bool paused, Clock::time_point now = Clock::now());

  private:
    std::chrono::seconds m_interval;
    Clock::time_point m_next;
  };
} // namespace desktop_stacks
