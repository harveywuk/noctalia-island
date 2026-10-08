#pragma once

#include "dbus/downloads/download_progress.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace island {
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
