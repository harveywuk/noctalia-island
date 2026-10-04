#include "launcher/snippet_matcher.h"

#include <algorithm>

void SnippetMatcher::setKeywords(std::vector<Keyword> keywords) {
  std::erase_if(keywords, [](const Keyword& entry) {
    return entry.keyword.size() < 2 || entry.keyword.find(' ') != std::string::npos;
  });
  std::ranges::stable_sort(keywords, [](const Keyword& a, const Keyword& b) {
    return a.keyword.size() > b.keyword.size();
  });
  m_keywords = std::move(keywords);
  m_buffer.clear();
}

std::optional<SnippetMatcher::Keyword> SnippetMatcher::feed(std::string_view utf8) {
  if (utf8.empty() || m_keywords.empty()) {
    return std::nullopt;
  }
  m_buffer.append(utf8);
  if (m_buffer.size() > kMaxBuffer) {
    // Drop whole code points from the front so the tail stays valid UTF-8.
    std::size_t cut = m_buffer.size() - kMaxBuffer;
    while (cut < m_buffer.size() && (static_cast<unsigned char>(m_buffer[cut]) & 0xC0) == 0x80) {
      ++cut;
    }
    m_buffer.erase(0, cut);
  }
  for (const Keyword& entry : m_keywords) {
    if (m_buffer.ends_with(entry.keyword)) {
      m_buffer.clear();
      return entry;
    }
  }
  return std::nullopt;
}

void SnippetMatcher::backspace() {
  if (m_buffer.empty()) {
    return;
  }
  std::size_t end = m_buffer.size() - 1;
  while (end > 0 && (static_cast<unsigned char>(m_buffer[end]) & 0xC0) == 0x80) {
    --end;
  }
  m_buffer.erase(end);
}

std::size_t SnippetMatcher::codePoints(std::string_view text) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}
