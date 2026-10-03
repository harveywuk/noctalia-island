#include "launcher/launcher_util.h"

#include "core/process/process.h"

#include <array>
#include <cmath>
#include <format>

namespace launcher_util {

  std::string urlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
      const auto byte = static_cast<unsigned char>(c);
      const bool unreserved = (byte >= 'A' && byte <= 'Z')
          || (byte >= 'a' && byte <= 'z')
          || (byte >= '0' && byte <= '9')
          || byte == '-'
          || byte == '_'
          || byte == '.'
          || byte == '~';
      if (unreserved) {
        out.push_back(c);
        continue;
      }
      out.push_back('%');
      out.push_back(kHex[byte >> 4U]);
      out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
  }

  std::string fileUri(const std::filesystem::path& path) {
    std::string out = "file://";
    for (const char c : path.string()) {
      if (c == '/') {
        out.push_back('/');
      } else {
        out += urlEncode(std::string_view(&c, 1));
      }
    }
    return out;
  }

  std::string fillUrlTemplate(std::string_view urlTemplate, std::string_view query) {
    constexpr std::string_view kPlaceholder = "{query}";
    std::string out(urlTemplate);
    const std::string encoded = urlEncode(query);
    for (std::size_t pos = out.find(kPlaceholder); pos != std::string::npos;
         pos = out.find(kPlaceholder, pos + encoded.size())) {
      out.replace(pos, kPlaceholder.size(), encoded);
    }
    return out;
  }

  bool openUri(const std::string& uri) {
    if (uri.empty()) {
      return false;
    }
    return process::runAsync(std::vector<std::string>{"xdg-open", uri});
  }

  bool showInFolder(const std::filesystem::path& path) {
    if (process::commandExists("gdbus")) {
      const std::string items = "['" + fileUri(path) + "']";
      if (process::runAsync(
              std::vector<std::string>{
                  "gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path",
                  "/org/freedesktop/FileManager1", "--method", "org.freedesktop.FileManager1.ShowItems", items, ""
              },
              process::RunCallbacks{
                  .onExit = [parent = path.parent_path().string()](process::RunResult result) {
                    // No file manager owns the name: fall back to opening the folder.
                    if (!result) {
                      (void)process::runAsync(std::vector<std::string>{"xdg-open", parent});
                    }
                  },
              }
          )) {
        return true;
      }
    }
    return openUri(path.parent_path().string());
  }

  std::string formatByteSize(std::uintmax_t bytes) {
    static constexpr std::array<const char*, 5> kUnits = {"B", "KB", "MB", "GB", "TB"};
    if (bytes < 1000) {
      return std::format("{} B", bytes);
    }
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < kUnits.size()) {
      value /= 1000.0;
      ++unit;
    }
    return value < 10.0 ? std::format("{:.1f} {}", value, kUnits[unit])
                        : std::format("{:.0f} {}", std::round(value), kUnits[unit]);
  }

} // namespace launcher_util
