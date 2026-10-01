#pragma once
#include "core/timer_manager.h"

#include <memory>
#include <string>
#include <vector>
class WaylandConnection;
class RenderContext;
namespace settings {
  class DisplayIdentifier {
  public:
    DisplayIdentifier(WaylandConnection&, RenderContext&);
    ~DisplayIdentifier();
    void show(const std::vector<std::string>& outputs);

  private:
    struct Instance;
    WaylandConnection& m_wayland;
    RenderContext& m_render;
    std::vector<std::unique_ptr<Instance>> m_instances;
    Timer m_timeout;
  };
} // namespace settings
