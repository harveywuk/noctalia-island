#pragma once

#include <cstdint>
#include <vector>

namespace visuals {

  // Apple Music's flowing artwork background, drawn on the CPU at a tiny size: four copies of
  // the (oversaturated) artwork at 125%, 80%, 50% and 25% of the width, the larger two spinning
  // in place and the smaller two spinning while circling, all through a gentle twist and then
  // blurred. Scaled up across a card, the low resolution reads as a soft, fluid wash.
  class ArtworkFlow {
  public:
    static constexpr int kWidth = 64;
    static constexpr int kHeight = 48;

    // `rgba` is a width x height RGBA8 image, typically the artwork centre-cropped to a square.
    bool setArtwork(const std::vector<std::uint8_t>& rgba, int width, int height);
    void clear();
    [[nodiscard]] bool hasArtwork() const noexcept { return !m_source.empty(); }

    // Renders the scene at `seconds` into `out` as kWidth x kHeight opaque RGBA8.
    void render(float seconds, std::vector<std::uint8_t>& out) const;

    // The artwork's most vivid colour, brightened enough to read on black, as 0..1 RGB; white
    // without artwork or when the artwork is greyscale.
    struct Accent {
      float r = 1, g = 1, b = 1;
    };
    [[nodiscard]] Accent accent() const noexcept { return m_accent; }

    // True when NOCTALIA_FREEZE_ARTWORK_FLOW is set: callers keep the flow still, so screenshot
    // tests can compare content drawn over it.
    [[nodiscard]] static bool frozen();

  private:
    struct Rgb {
      float r = 0, g = 0, b = 0;
    };
    [[nodiscard]] Rgb sample(float u, float v) const;

    std::vector<Rgb> m_source;
    int m_width = 0;
    int m_height = 0;
    Rgb m_average;
    Accent m_accent;
  };

} // namespace visuals
