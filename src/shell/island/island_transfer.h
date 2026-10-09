#pragma once

#include "dbus/downloads/download_progress.h"
#include "render/core/render_styles.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace island {
  inline std::optional<RingColors> transferColors(std::span<const DownloadProgress> transfers, bool motion) {
    if (!motion || transfers.size() != 1 || transfers.front().paused() || !transfers.front().leds)
      return std::nullopt;
    const auto& frame = *transfers.front().leds;
    RingColors colors;
    for (std::size_t i = 0; i < colors.size(); ++i) {
      // Steam fills its bar from pixel 16 towards 0. Start that end at the top
      // of our ring and preserve the colour and brightness of every light.
      const auto& pixel = frame.pixels[colors.size() - 1 - i];
      const auto level = std::max({pixel[0], pixel[1], pixel[2]});
      colors[i] = level == 0 ? rgba(0, 0, 0, 0)
                             : rgba(
                                   static_cast<float>(pixel[0]) / level, static_cast<float>(pixel[1]) / level,
                                   static_cast<float>(pixel[2]) / level,
                                   colorByte(level) * colorByte(pixel[3]) * colorByte(frame.brightness)
                               );
    }
    return colors;
  }

  inline std::string transferGlyph(const DownloadProgress& transfer) {
    if (transfer.paused())
      return "media-pause";
    if (!transfer.icon.empty())
      return transfer.icon;
    if (transfer.phase == "installing")
      return "package";
    if (transfer.phase == "verifying")
      return "file-check";
    return "download";
  }

  inline std::string transferGlyph(std::span<const DownloadProgress> transfers) {
    if (transfers.size() == 1)
      return transferGlyph(transfers.front());
    return downloadsPaused(transfers) ? "media-pause" : "download";
  }

  // Jobs from one app still have distinct identities. Forget a finished lead so
  // a later job cannot inherit a choice made for an earlier transfer.
  inline void orderTransfers(std::vector<DownloadProgress>& transfers, std::string& leadKey) {
    if (leadKey.empty())
      return;
    const auto lead = std::ranges::find(transfers, leadKey, &DownloadProgress::key);
    if (lead == transfers.end())
      leadKey.clear();
    else
      std::rotate(transfers.begin(), lead, lead + 1);
  }

  enum class TransferStatus { Running, Paused, Failed };
  enum class TransferNotice { DownloadFinished, TransferFinished, Failed };

  struct TransferFeedback {
    TransferNotice notice;
    DownloadSource source;
    // A fresh result replaces the action as well as the text. Pointer/key
    // gestures begun for an older result must never activate its replacement.
    std::uint64_t serial;
    // Display metadata only; activation always uses the source app identity.
    std::string title;
    std::string detail;
  };

  class RecentTransfers {
  public:
    using Clock = std::chrono::steady_clock;
    struct Entry {
      TransferFeedback feedback;
      Clock::time_point received;
    };
    void remember(TransferFeedback feedback, Clock::time_point now = Clock::now()) {
      m_entries.insert(m_entries.begin(), {std::move(feedback), now});
      if (m_entries.size() > 3)
        m_entries.pop_back();
    }
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry* find(std::uint64_t serial) const {
      const auto found =
          std::ranges::find_if(m_entries, [serial](const auto& entry) { return entry.feedback.serial == serial; });
      return found == m_entries.end() ? nullptr : &*found;
    }
    bool empty() const { return m_entries.empty(); }

  private:
    std::vector<Entry> m_entries;
  };

  inline std::optional<TransferStatus> transferStatus(std::string_view status) {
    if (status == "running")
      return TransferStatus::Running;
    if (status == "paused")
      return TransferStatus::Paused;
    if (status == "failed")
      return TransferStatus::Failed;
    return std::nullopt;
  }
} // namespace island
