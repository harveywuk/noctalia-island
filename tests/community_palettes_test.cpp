#include "theme/community_palettes.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {
  double luminance(const std::string& color) {
    const auto channel = [&](int offset) {
      const double c = std::stoi(color.substr(offset, 2), nullptr, 16) / 255.0;
      return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
    };
    return .2126 * channel(1) + .7152 * channel(3) + .0722 * channel(5);
  }
  double contrast(const std::string& a, const std::string& b) {
    const auto x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
  }
} // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  namespace fs = std::filesystem;
  using namespace noctalia::theme;
  const auto root = fs::temp_directory_path() / ("noctalia-community-test-" + std::to_string(getpid()));
  setenv("NOCTALIA_STATE_HOME", root.c_str(), 1);
  setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);
  const auto bundled = bundledCommunityPalettePath("macOS");
  assert(fs::is_regular_file(bundled));
  assert(bundledCommunityPalettePath("../macOS").empty());
  assert(bundledCommunityPalettePath("Remote").empty());
  auto catalog = availableCommunityPalettes();
  assert(std::ranges::count(catalog, "macOS", &AvailablePalette::name) == 1);
  assert(!fs::exists(communityPaletteCacheDir())); // Offline discovery does not mutate state.

  const auto cache = communityPaletteCacheDir() / ".catalog/palettes.json";
  fs::create_directories(cache.parent_path());
  std::ofstream(cache) << R"([{"name":"Remote","md5":"abc"},{"name":"macOS","md5":"remote-copy"}])";
  catalog = availableCommunityPalettes();
  // The remote entry plus every bundled palette; the remote macOS copy defers to the bundled one.
  const auto bundledCatalog =
      nlohmann::json::parse(std::ifstream(fs::path(argv[1]) / "community-palettes/catalog.json"));
  assert(catalog.size() == 1 + bundledCatalog.size());
  const auto mac = std::ranges::find(catalog, "macOS", &AvailablePalette::name);
  assert(mac != catalog.end() && mac->md5.empty());
  assert(!mac->preview.dark.accents.empty() && !mac->preview.light.accents.empty());
  assert(communityPaletteCatalogMd5("Remote") == "abc");

  // Every bundled palette, not only macOS, has both modes and meets the same contrast pairs.
  assert(bundledCatalog.size() >= 5);
  for (const auto& entry : bundledCatalog) {
    const auto palettePath = bundledCommunityPalettePath(entry.at("name").get<std::string>());
    assert(fs::is_regular_file(palettePath));
    const auto palette = nlohmann::json::parse(std::ifstream(palettePath));
    for (const auto* mode : {"dark", "light"}) {
      const auto& colors = palette.at(mode);
      for (const auto* role :
           {"primary", "secondary", "tertiary", "error", "surface", "surfaceVariant", "onPrimary", "onSecondary",
            "onTertiary", "onError", "onSurface", "onSurfaceVariant", "outline", "shadow", "hover", "onHover"}) {
        assert(colors.at(role).get<std::string>().size() == 7);
      }
      for (const auto& pair :
           {std::pair{"onSurface", "surface"},
            {"onSurface", "surfaceVariant"},
            {"onSurfaceVariant", "surfaceVariant"},
            {"onPrimary", "primary"},
            {"onError", "error"},
            {"onHover", "hover"}}) {
        assert(contrast(colors.at(pair.first), colors.at(pair.second)) >= 4.5);
      }
      const auto& terminal = colors.at("terminal");
      assert(terminal.at("normal").size() == 8 && terminal.at("bright").size() == 8);
      assert(contrast(terminal.at("foreground"), terminal.at("background")) >= 4.5);
    }
  }
  // A damaged remote catalog must not hide the bundled preset.
  std::ofstream(cache) << "broken";
  assert(std::ranges::count(availableCommunityPalettes(), "macOS", &AvailablePalette::name) == 1);
  fs::remove_all(root);
}
