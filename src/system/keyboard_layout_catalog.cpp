#include "system/keyboard_layout_catalog.h"

#include <algorithm>
#include <fstream>
#include <sstream>

std::vector<KeyboardLayoutChoice> parseKeyboardLayouts(std::istream& stream) {
  std::vector<KeyboardLayoutChoice> result;
  std::string section, line;
  while (std::getline(stream, line)) {
    std::istringstream row(line);
    std::string key;
    row >> key;
    if (key == "!") {
      row >> section;
      continue;
    }
    if (key.empty() || (section != "layout" && section != "variant"))
      continue;
    std::string label;
    if (section == "variant") {
      std::string layout;
      row >> layout;
      if (!layout.ends_with(':'))
        continue;
      layout.pop_back();
      key = layout + ":" + key;
    }
    std::getline(row >> std::ws, label);
    if (label.empty())
      continue;
    if (std::ranges::none_of(result, [&](const auto& choice) { return choice.value == key; }))
      result.push_back({std::move(key), std::move(label)});
  }
  std::ranges::sort(result, {}, &KeyboardLayoutChoice::label);
  return result;
}
const std::vector<KeyboardLayoutChoice>& keyboardLayoutCatalog() {
  static const auto choices = [] {
    std::ifstream file("/usr/share/X11/xkb/rules/evdev.lst");
    auto rows = parseKeyboardLayouts(file);
    if (rows.empty())
      rows.push_back({"us", "English (US)"});
    return rows;
  }();
  return choices;
}
