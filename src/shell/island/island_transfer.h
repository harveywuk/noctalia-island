#pragma once

#include "dbus/downloads/download_progress.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace island {
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
