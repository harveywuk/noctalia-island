#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class HttpClient;

namespace noctalia::theme {

  struct AvailablePalette {
    struct PreviewMode {
      std::string surface;
      std::vector<std::string> accents;
    };

    struct Preview {
      PreviewMode dark;
      PreviewMode light;
    };

    std::string name;
    std::string md5;
    Preview preview = {};
  };

  class CommunityPaletteService {
  public:
    using ReadyCallback = std::function<void()>;

    explicit CommunityPaletteService(HttpClient& httpClient);

    void setReadyCallback(ReadyCallback callback);
    void sync();

  private:
    HttpClient& m_httpClient;
    ReadyCallback m_readyCallback;
    std::uint64_t m_generation = 0;
  };

  [[nodiscard]] std::vector<AvailablePalette> availableCommunityPalettes();

  // The palettes to offer in pickers: with curatedOnly, just the bundled presets (whose contrast is
  // checked) plus `selected` when it is a catalog palette chosen earlier, so it stays visible;
  // otherwise every available palette.
  [[nodiscard]] std::vector<AvailablePalette> communityPalettesForPicker(bool curatedOnly, std::string_view selected);

  // Bundled community presets remain selectable offline and are never downloaded over.
  // Returns an empty path for names that are not in the bundled catalog.
  [[nodiscard]] std::filesystem::path bundledCommunityPalettePath(std::string_view name);

  // Catalog-declared MD5 of the named palette's downloaded JSON, or empty if the
  // palette is absent from the cached catalog or the catalog omits a checksum.
  [[nodiscard]] std::string communityPaletteCatalogMd5(std::string_view name);

  [[nodiscard]] std::filesystem::path communityPaletteCacheDir();
  [[nodiscard]] std::filesystem::path communityPaletteCachePath(std::string_view name);
  [[nodiscard]] std::string communityPaletteDownloadUrl(std::string_view name);

} // namespace noctalia::theme
