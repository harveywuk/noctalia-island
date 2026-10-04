#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The typed-text side of snippet expansion: a short buffer of what was typed since the last
// word break, matched against snippet keywords after every character. Pure logic, no input
// devices, so it is unit-tested on its own.
class SnippetMatcher {
public:
  struct Keyword {
    std::string keyword;
    std::string snippetId;
  };

  // Keeps the keywords that can expand (at least two characters, no spaces).
  void setKeywords(std::vector<Keyword> keywords);
  [[nodiscard]] bool empty() const noexcept { return m_keywords.empty(); }

  // A printable character (one UTF-8 code point) was typed; returns the keyword the buffer now ends
  // with, longest first, and resets the buffer when one matched.
  std::optional<Keyword> feed(std::string_view utf8);
  // Backspace: drops the last code point.
  void backspace();
  // Anything else (Return, arrows, a modifier chord, focus change) ends the word.
  void reset() noexcept { m_buffer.clear(); }
  [[nodiscard]] const std::string& buffer() const noexcept { return m_buffer; }

  // Code points in a UTF-8 string: how many backspaces it takes to remove a typed keyword.
  [[nodiscard]] static std::size_t codePoints(std::string_view text) noexcept;

private:
  static constexpr std::size_t kMaxBuffer = 64;

  std::vector<Keyword> m_keywords; // longest keyword first
  std::string m_buffer;
};
