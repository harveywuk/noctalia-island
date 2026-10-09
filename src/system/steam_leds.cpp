#include "system/steam_leds.h"

#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

std::optional<SteamLedFrame> decodeSteamLeds(std::span<const std::uint8_t> bytes, std::uint64_t monotonicNs) {
  if (bytes.size() != 100)
    return std::nullopt;
  // The kernel ABI uses native-endian packed integers. Copy instead of making
  // unaligned accesses or assuming the host's struct padding matches the wire.
  std::uint32_t magic;
  std::uint16_t version, size;
  std::uint64_t timestamp;
  std::memcpy(&magic, bytes.data(), sizeof(magic));
  std::memcpy(&version, bytes.data() + 4, sizeof(version));
  std::memcpy(&size, bytes.data() + 6, sizeof(size));
  std::memcpy(&timestamp, bytes.data() + 16, sizeof(timestamp));
  if (magic != 0x564c4544 || version != 1 || size != 100 || bytes[24] != 1 || bytes[25] != 1 || bytes[26] == 0)
    return std::nullopt;
  // Hardware effects do not publish their generated frames. Only manual output
  // is usable, and old snapshots must not linger after Steam stops writing.
  if (timestamp == 0 || timestamp > monotonicNs || monotonicNs - timestamp > 5'000'000'000ULL)
    return std::nullopt;
  SteamLedFrame frame;
  frame.brightness = bytes[26];
  for (std::size_t i = 0; i < frame.pixels.size(); ++i)
    std::memcpy(frame.pixels[i].data(), bytes.data() + 32 + i * 4, 4);
  return frame;
}

std::optional<SteamLedFrame> SteamLeds::read() const {
  // One bounded snapshot per sample. Do not drain this device: every read
  // returns a snapshot, even when nothing changed. Close so module unloads work.
  const int fd = open(m_device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return std::nullopt;
  struct stat info{};
  std::array<std::uint8_t, 101> bytes{};
  const auto count = fstat(fd, &info) == 0 && (S_ISCHR(info.st_mode) || S_ISREG(info.st_mode))
      ? ::read(fd, bytes.data(), bytes.size())
      : -1;
  close(fd);
  timespec now{};
  if (count != 100 || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return std::nullopt;
  return decodeSteamLeds(
      std::span(bytes).first(100),
      static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(now.tv_nsec)
  );
}
