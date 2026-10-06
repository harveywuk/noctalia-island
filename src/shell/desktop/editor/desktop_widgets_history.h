#pragma once

#include "config/config_types.h"

#include <chrono>
#include <string>
#include <vector>

// A drag records only its final snapshot;
// repeated changes to the same setting coalesce until another interaction begins.
class DesktopWidgetsHistory {
public:
  [[nodiscard]] bool matches(const DesktopWidgetsConfig& snapshot) const {
    return !m_entries.empty() && m_entries[m_cursor] == snapshot;
  }
  void replaceCurrent(const DesktopWidgetsConfig& snapshot) {
    if (m_entries.empty())
      reset(snapshot);
    else
      m_entries[m_cursor] = snapshot;
    breakGroup();
  }
  void reset(const DesktopWidgetsConfig& snapshot) {
    m_entries = {snapshot};
    m_cursor = 0;
    breakGroup();
  }
  void breakGroup() { m_group.clear(); }
  void record(const DesktopWidgetsConfig& snapshot, const std::string& group = {}) {
    if (m_entries.empty()) {
      reset(snapshot);
      return;
    }
    if (snapshot == m_entries[m_cursor])
      return;
    const auto now = std::chrono::steady_clock::now();
    const bool merge = !group.empty()
        && group == m_group
        && m_cursor > 0
        && !canRedo()
        && now - m_lastChange < std::chrono::seconds(2);
    m_entries.resize(m_cursor + 1);
    if (merge) {
      if (snapshot == m_entries[m_cursor - 1]) {
        m_entries.pop_back();
        --m_cursor;
        breakGroup();
        return;
      }
      m_entries[m_cursor] = snapshot;
    } else {
      m_entries.push_back(snapshot);
      if (m_entries.size() > 101)
        m_entries.erase(m_entries.begin());
      m_cursor = m_entries.size() - 1;
    }
    m_group = group;
    m_lastChange = now;
  }
  [[nodiscard]] bool canUndo() const { return m_cursor > 0; }
  [[nodiscard]] bool canRedo() const { return m_cursor + 1 < m_entries.size(); }
  [[nodiscard]] const DesktopWidgetsConfig* undo() {
    breakGroup();
    return canUndo() ? &m_entries[--m_cursor] : nullptr;
  }
  [[nodiscard]] const DesktopWidgetsConfig* redo() {
    breakGroup();
    return canRedo() ? &m_entries[++m_cursor] : nullptr;
  }

private:
  std::vector<DesktopWidgetsConfig> m_entries;
  std::size_t m_cursor = 0;
  std::string m_group;
  std::chrono::steady_clock::time_point m_lastChange;
};
