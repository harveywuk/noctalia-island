#include "tests/test_check.h"
#include "ui/visuals/artwork_flow.h"

#include <cstdint>
#include <vector>

int main() {
  using visuals::ArtworkFlow;
  ArtworkFlow flow;
  std::vector<std::uint8_t> frame;

  // Without artwork the frame is opaque black.
  flow.render(0.0F, frame);
  TEST_CHECK(frame.size() == static_cast<std::size_t>(ArtworkFlow::kWidth * ArtworkFlow::kHeight * 4));
  TEST_CHECK(frame[0] == 0 && frame[3] == 255);

  // A two-tone artwork: the left half red, the right half blue.
  constexpr int size = 8;
  std::vector<std::uint8_t> art(size * size * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = &art[static_cast<std::size_t>(y * size + x) * 4];
      p[0] = x < size / 2 ? 220 : 20;
      p[2] = x < size / 2 ? 20 : 220;
      p[3] = 255;
    }
  TEST_CHECK(!flow.setArtwork(art, size, size + 1)); // Too few bytes.
  TEST_CHECK(!flow.hasArtwork());
  TEST_CHECK(flow.setArtwork(art, size, size));

  std::vector<std::uint8_t> later;
  flow.render(0.0F, frame);
  flow.render(20.0F, later);
  // Both artwork colours appear, dimmed below full brightness, and the scene moves over time.
  bool reddish = false, bluish = false;
  for (std::size_t i = 0; i < frame.size(); i += 4) {
    reddish = reddish || frame[i] > frame[i + 2] + 40;
    bluish = bluish || frame[i + 2] > frame[i] + 40;
    TEST_CHECK(frame[i] < 255 && frame[i + 2] < 255 && frame[i + 3] == 255);
  }
  TEST_CHECK(reddish && bluish);
  TEST_CHECK(frame != later);
  // Rendering is deterministic for a given time.
  std::vector<std::uint8_t> again;
  flow.render(20.0F, again);
  TEST_CHECK(again == later);

  // The accent is the artwork's vivid colour, lifted to read on black: red or blue here.
  const auto accent = flow.accent();
  TEST_CHECK((accent.r > 0.8F && accent.b < 0.3F) || (accent.b > 0.8F && accent.r < 0.3F));

  // Greyscale artwork has no accent: white.
  std::vector<std::uint8_t> grey(size * size * 4, 128);
  TEST_CHECK(flow.setArtwork(grey, size, size));
  TEST_CHECK(flow.accent().r == 1.0F && flow.accent().g == 1.0F && flow.accent().b == 1.0F);

  flow.clear();
  TEST_CHECK(!flow.hasArtwork());
  TEST_CHECK(flow.accent().r == 1.0F);
  return 0;
}
