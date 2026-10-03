// Covers the launcher's Spotlight-style file search: what the home-folder walk indexes and how a
// query ranks names.

#include "launcher/file_provider.h"
#include "tests/test_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  }

  [[nodiscard]] bool indexed(const std::vector<FileProvider::Entry>& entries, std::string_view name) {
    return std::ranges::any_of(entries, [name](const FileProvider::Entry& e) {
      return std::string_view(e.path).substr(e.nameOffset) == name;
    });
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-file-provider-" + std::to_string(::getpid()));
  fs::remove_all(root);
  touch(root / "Documents" / "Quarterly Report.pdf");
  touch(root / "Documents" / "Archive" / "report-draft.txt");
  touch(root / "Pictures" / "holiday.png");
  touch(root / ".config" / "report.conf");
  touch(root / "Code" / "app" / "node_modules" / "report" / "index.js");
  touch(root / "a" / "b" / "c" / "d" / "e" / "f" / "g" / "deep-report.txt");

  const auto entries = FileProvider::scan(root);
  TEST_CHECK(indexed(entries, "Quarterly Report.pdf"));
  TEST_CHECK(indexed(entries, "report-draft.txt"));
  TEST_CHECK(indexed(entries, "Documents"));
  // Dotfolders, dependency trees and very deep paths stay out of the index.
  TEST_CHECK(!indexed(entries, "report.conf"));
  TEST_CHECK(!indexed(entries, "index.js"));
  TEST_CHECK(!indexed(entries, "deep-report.txt"));

  const std::string home = root.string();
  const auto results = FileProvider::search(entries, "report", 10, home);
  TEST_CHECK(results.size() == 2);
  // A name that starts with the query outranks one that only contains it.
  TEST_CHECK(results[0].title == "report-draft.txt");
  TEST_CHECK(results[1].title == "Quarterly Report.pdf");
  TEST_CHECK(results[1].subtitle == "~/Documents");
  TEST_CHECK(results[1].iconName == "application-pdf");

  const auto folders = FileProvider::search(entries, "docu", 10, home);
  TEST_CHECK(folders.size() == 1 && folders[0].iconName == "folder");

  // Scattered letters are not a match, and one character is too short to search.
  TEST_CHECK(FileProvider::search(entries, "qrp", 10, home).empty());
  TEST_CHECK(FileProvider::search(entries, "r", 10, home).empty());
  TEST_CHECK(FileProvider::search(entries, "report", 1, home).size() == 1);

  fs::remove_all(root);
  return 0;
}
