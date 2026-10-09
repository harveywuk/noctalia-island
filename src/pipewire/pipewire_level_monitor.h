#pragma once

#include <memory>
#include <string>

class PipeWireService;

// A passive input meter. Samples are reduced to a peak in place and never retained.
// Own this only while a visible control needs levels; it must not keep capture active.
class PipeWireLevelMonitor {
public:
  PipeWireLevelMonitor(PipeWireService& service, const std::string& target);
  ~PipeWireLevelMonitor();
  PipeWireLevelMonitor(const PipeWireLevelMonitor&) = delete;
  PipeWireLevelMonitor& operator=(const PipeWireLevelMonitor&) = delete;

  [[nodiscard]] float level() const;

private:
  class Stream;
  std::unique_ptr<Stream> m_stream;
};
