#include "ui/visuals/artwork_flow.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace visuals {

  namespace {

    constexpr float kSaturation = 1.6F;
    // Dimmed so white text and controls read over any artwork.
    constexpr float kBrightness = 0.72F;
    constexpr int kBlurPasses = 3;
    constexpr int kBlurRadius = 2;

    struct Layer {
      float size;       // Fraction of the output width.
      float spin;       // Radians per second.
      float orbit;      // Orbit radius as a fraction of the width; 0 spins in place.
      float orbitSpeed; // Radians per second.
      float phase;
    };

    // Back to front, as on the reference: the two largest spin in place, the two smallest also
    // travel along circles.
    constexpr std::array<Layer, 4> kLayers{{
        {1.25F, 0.05F, 0.0F, 0.0F, 0.0F},
        {0.8F, -0.07F, 0.0F, 0.0F, 1.3F},
        {0.5F, 0.11F, 0.24F, 0.09F, 2.1F},
        {0.25F, -0.14F, 0.32F, -0.12F, 4.2F},
    }};

  } // namespace

  bool ArtworkFlow::setArtwork(const std::vector<std::uint8_t>& rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4) {
      clear();
      return false;
    }
    m_width = width;
    m_height = height;
    m_source.assign(static_cast<std::size_t>(width) * height, {});
    Rgb sum;
    for (std::size_t i = 0; i < m_source.size(); ++i) {
      Rgb c{rgba[i * 4] / 255.0F, rgba[i * 4 + 1] / 255.0F, rgba[i * 4 + 2] / 255.0F};
      // Oversaturate around the pixel's luminance, as the reference does to keep colours vivid.
      const float luma = 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
      c = {std::clamp(luma + (c.r - luma) * kSaturation, 0.0F, 1.0F),
           std::clamp(luma + (c.g - luma) * kSaturation, 0.0F, 1.0F),
           std::clamp(luma + (c.b - luma) * kSaturation, 0.0F, 1.0F)};
      m_source[i] = c;
      sum.r += c.r;
      sum.g += c.g;
      sum.b += c.b;
    }
    const float count = static_cast<float>(m_source.size());
    m_average = {sum.r / count, sum.g / count, sum.b / count};
    return true;
  }

  void ArtworkFlow::clear() {
    m_source.clear();
    m_width = m_height = 0;
    m_average = {};
  }

  ArtworkFlow::Rgb ArtworkFlow::sample(float u, float v) const {
    // Bilinear, clamped to the edge.
    const float x = std::clamp(u * static_cast<float>(m_width) - 0.5F, 0.0F, static_cast<float>(m_width - 1));
    const float y = std::clamp(v * static_cast<float>(m_height) - 0.5F, 0.0F, static_cast<float>(m_height - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, m_width - 1);
    const int y1 = std::min(y0 + 1, m_height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto at = [&](int px, int py) { return m_source[static_cast<std::size_t>(py) * m_width + px]; };
    const auto mix = [](Rgb a, Rgb b, float t) {
      return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    };
    return mix(mix(at(x0, y0), at(x1, y0), fx), mix(at(x0, y1), at(x1, y1), fx), fy);
  }

  void ArtworkFlow::render(float seconds, std::vector<std::uint8_t>& out) const {
    std::vector<Rgb> frame(static_cast<std::size_t>(kWidth) * kHeight, m_average);
    if (hasArtwork()) {
      const float aspect = static_cast<float>(kHeight) / static_cast<float>(kWidth);
      // The twist: coordinates near a slowly wandering point rotate more the closer they are.
      const float twistX = 0.5F + 0.18F * std::sin(seconds * 0.07F);
      const float twistY = aspect * (0.5F + 0.2F * std::cos(seconds * 0.05F));
      constexpr float kTwistRadius = 0.6F;
      constexpr float kTwistAngle = 1.4F;
      for (int py = 0; py < kHeight; ++py) {
        for (int px = 0; px < kWidth; ++px) {
          float x = (static_cast<float>(px) + 0.5F) / kWidth;
          float y = (static_cast<float>(py) + 0.5F) / kWidth;
          const float dx = x - twistX;
          const float dy = y - twistY;
          const float dist = std::sqrt(dx * dx + dy * dy);
          if (dist < kTwistRadius) {
            const float ratio = (kTwistRadius - dist) / kTwistRadius;
            const float angle = ratio * ratio * kTwistAngle;
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            x = twistX + dx * c - dy * s;
            y = twistY + dx * s + dy * c;
          }
          Rgb colour = m_average;
          for (const auto& layer : kLayers) {
            const float orbitAngle = layer.phase + seconds * layer.orbitSpeed;
            const float cx = 0.5F + layer.orbit * std::cos(orbitAngle);
            const float cy = aspect * 0.5F + layer.orbit * std::sin(orbitAngle);
            const float angle = -(layer.phase + seconds * layer.spin);
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            const float lx = x - cx;
            const float ly = y - cy;
            const float u = (lx * c - ly * s) / layer.size + 0.5F;
            const float v = (lx * s + ly * c) / layer.size + 0.5F;
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
              colour = sample(u, v);
          }
          frame[static_cast<std::size_t>(py) * kWidth + px] = colour;
        }
      }
      // A separable box blur, repeated, approximates the reference's Kawase blur.
      std::vector<Rgb> scratch(frame.size());
      for (int pass = 0; pass < kBlurPasses; ++pass) {
        for (int horizontal = 1; horizontal >= 0; --horizontal) {
          for (int py = 0; py < kHeight; ++py) {
            for (int px = 0; px < kWidth; ++px) {
              Rgb sum;
              for (int k = -kBlurRadius; k <= kBlurRadius; ++k) {
                const int sx = horizontal ? std::clamp(px + k, 0, kWidth - 1) : px;
                const int sy = horizontal ? py : std::clamp(py + k, 0, kHeight - 1);
                const auto& c = frame[static_cast<std::size_t>(sy) * kWidth + sx];
                sum.r += c.r;
                sum.g += c.g;
                sum.b += c.b;
              }
              constexpr float kTaps = 2 * kBlurRadius + 1;
              scratch[static_cast<std::size_t>(py) * kWidth + px] = {sum.r / kTaps, sum.g / kTaps, sum.b / kTaps};
            }
          }
          frame.swap(scratch);
        }
      }
    }
    out.resize(frame.size() * 4);
    for (std::size_t i = 0; i < frame.size(); ++i) {
      const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value * kBrightness, 0.0F, 1.0F) * 255.0F));
      };
      out[i * 4] = byte(frame[i].r);
      out[i * 4 + 1] = byte(frame[i].g);
      out[i * 4 + 2] = byte(frame[i].b);
      out[i * 4 + 3] = 255;
    }
  }

} // namespace visuals
