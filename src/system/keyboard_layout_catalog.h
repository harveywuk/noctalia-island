#pragma once
#include <istream>
#include <string>
#include <vector>

struct KeyboardLayoutChoice {
  std::string value, label;
};
std::vector<KeyboardLayoutChoice> parseKeyboardLayouts(std::istream&);
const std::vector<KeyboardLayoutChoice>& keyboardLayoutCatalog();
