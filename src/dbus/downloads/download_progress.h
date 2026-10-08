#pragma once

#include "system/desktop_entry.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>

struct DownloadSource {
  std::string desktopId;
  std::string name;
  std::string wmClass;
};

// Keep the desktop identity and WM class together. Display names are never
// used to guess a target window, and no desktop Exec command is launched.
inline DownloadSource downloadSource(const std::string& desktopId, std::span<const DesktopEntry> entries) {
  const auto id = desktopId.ends_with(".desktop") ? desktopId.substr(0, desktopId.size() - 8) : desktopId;
  const auto found =
      std::ranges::find_if(entries, [&](const auto& entry) { return entry.id == id || entry.id == desktopId; });
  if (found != entries.end())
    return {id, found->name, found->startupWmClass};
  return {id, id, {}};
}

struct DownloadProgress {
  std::string desktopId;
  std::string name;
  double progress = 0;
  bool determinate = true;
  std::string phase;
  // Empty means the download arrow. Scripts can set their own symbol.
  std::string icon;
  // Distinguish publishers and Steam games even when they share an app.
  std::string key;
  DownloadSource source;
  bool paused() const { return phase == "paused"; }
};

inline bool downloadsPaused(std::span<const DownloadProgress> downloads) {
  return !downloads.empty() && std::ranges::all_of(downloads, &DownloadProgress::paused);
}

inline std::optional<float> downloadFraction(std::span<const DownloadProgress> downloads) {
  if (downloads.empty())
    return std::nullopt;
  if (!std::ranges::all_of(downloads, [](const auto& item) { return item.determinate; })) {
    // An unknown total must not spin while all transfers are explicitly paused.
    return downloadsPaused(downloads) ? std::optional{0.0F} : std::nullopt;
  }
  double total = 0;
  for (const auto& item : downloads)
    total += item.progress;
  return static_cast<float>(total / static_cast<double>(downloads.size()));
}
