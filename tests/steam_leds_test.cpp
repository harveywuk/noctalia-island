#include "shell/island/island_transfer.h"
#include "system/steam_leds.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

int main() {
  timespec now{};
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  const auto ns = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL + now.tv_nsec;
  std::vector<std::uint8_t> bytes(100);
  const auto put = [&](std::size_t at, auto value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); };
  put(0, std::uint32_t{0x564c4544});
  put(4, std::uint16_t{1});
  put(6, std::uint16_t{100});
  put(8, std::uint64_t{42});
  put(16, ns);
  bytes[24] = bytes[25] = 1;
  bytes[26] = 55;
  for (std::size_t i = 0; i < 17; ++i) {
    bytes[32 + i * 4 + 3] = 255;
    if (i >= 9) {
      bytes[32 + i * 4] = 1;
      bytes[32 + i * 4 + 1] = 90;
      bytes[32 + i * 4 + 2] = 255;
    }
  }
  auto frame = decodeSteamLeds(bytes, ns);
  assert(frame && frame->brightness == 55);
  assert(frame->pixels[8][2] == 0 && frame->pixels[16][2] == 255);
  assert(!decodeSteamLeds(std::span(bytes).first(99), ns));
  bytes.push_back(0);
  assert(!decodeSteamLeds(bytes, ns));
  bytes.pop_back();
  for (auto at : {0U, 4U, 6U, 24U, 25U}) {
    const auto old = bytes[at];
    bytes[at] = 9;
    assert(!decodeSteamLeds(bytes, ns));
    bytes[at] = old;
  }
  bytes[26] = 0;
  assert(!decodeSteamLeds(bytes, ns));
  bytes[26] = 55;
  assert(!decodeSteamLeds(bytes, ns - 1));
  assert(!decodeSteamLeds(bytes, ns + 5'000'000'001ULL));
  assert(decodeSteamLeds(bytes, ns + 5'000'000'000ULL));

  DownloadProgress transfer{.determinate = false, .phase = "downloading", .leds = frame};
  auto colors = island::transferColors(std::array{transfer}, true);
  assert(colors && (*colors)[0].b == 1 && (*colors)[0].a > 0 && (*colors)[8].a == 0);
  assert(!downloadFraction(std::array{transfer})); // LED output never becomes a claimed percentage.
  assert(!island::transferColors(std::array{transfer, transfer}, true));
  assert(!island::transferColors(std::array{transfer}, false)); // Reduced motion retains the static fallback.
  transfer.phase = "paused";
  assert(!island::transferColors(std::array{transfer}, true));
  transfer.phase = "downloading";
  transfer.leds.reset();
  assert(!island::transferColors(std::array{transfer}, true));

  char path[] = "/tmp/noctalia-steam-led-test-XXXXXX";
  assert(mkdtemp(path));
  const auto device = std::filesystem::path(path) / "snapshot";
  SteamLeds reader(device);
  assert(!reader.read());
  const auto write = [&] {
    std::ofstream file(device, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  };
  write();
  assert(reader.read() == frame);
  bytes[32 + 16 * 4 + 2] = 200;
  write();
  assert(reader.read()->pixels[16][2] == 200); // Read from the start on every sample.
  bytes.resize(8);
  write();
  assert(!reader.read());
  std::filesystem::remove(device);
  assert(mkfifo(device.c_str(), 0600) == 0);
  assert(!reader.read()); // Never wait for a writer on a wrong device type.
  std::filesystem::remove(device);
  std::filesystem::create_symlink("/dev/zero", device);
  assert(!reader.read());
  std::filesystem::remove_all(path);
}
