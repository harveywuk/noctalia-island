#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>

// Version 1 of the optional leds-valve-shim snapshot ABI. These are LED colours,
// not byte counts, percentages, app identities or completion notifications.
struct SteamLedFrame {
  std::array<std::array<std::uint8_t, 4>, 17> pixels{};
  std::uint8_t brightness = 0;
  bool operator==(const SteamLedFrame&) const = default;
};

std::optional<SteamLedFrame> decodeSteamLeds(std::span<const std::uint8_t> bytes, std::uint64_t monotonicNs);

class SteamLeds {
public:
  explicit SteamLeds(std::filesystem::path device = "/dev/valve-leds-shim") : m_device(std::move(device)) {}
  [[nodiscard]] std::optional<SteamLedFrame> read() const;

private:
  std::filesystem::path m_device;
};
