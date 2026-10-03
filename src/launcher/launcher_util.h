#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Small helpers shared by the launcher providers' actions.
namespace launcher_util {

  // Percent-encodes everything outside RFC 3986's unreserved set; spaces become %20.
  [[nodiscard]] std::string urlEncode(std::string_view text);
  // A file:// URI for an absolute path.
  [[nodiscard]] std::string fileUri(const std::filesystem::path& path);
  // Substitutes {query} (percent-encoded) into a URL template. A template without {query}
  // is returned unchanged.
  [[nodiscard]] std::string fillUrlTemplate(std::string_view urlTemplate, std::string_view query);

  // Opens a URL or path with the desktop's default handler.
  bool openUri(const std::string& uri);
  // Reveals a file in the file manager (org.freedesktop.FileManager1), or opens its folder.
  bool showInFolder(const std::filesystem::path& path);

  // "512 B", "12 KB", "3.4 MB" (decimal units, like Finder and Raycast).
  [[nodiscard]] std::string formatByteSize(std::uintmax_t bytes);

} // namespace launcher_util
