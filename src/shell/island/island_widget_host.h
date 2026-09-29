#pragma once

#include "render/scene/node.h"
#include "shell/bar/widget.h"

#include <functional>
#include <memory>
#include <vector>

class WidgetFactory;
struct Config;
struct wl_output;
namespace noctalia::bar {
  class WidgetActionDispatcher;
}

// Retains ordinary bar widgets while the surrounding hover card is rebuilt.
class IslandWidgetHost : public Node {
public:
  IslandWidgetHost(
      WidgetFactory&, const Config&, wl_output*, float scale, AnimationManager*,
      const noctalia::bar::WidgetActionDispatcher*, std::function<void()> update, std::function<void()> redraw,
      std::function<void()> frame
  );
  ~IslandWidgetHost() override;
  void updateWidgets(Renderer&, float availableWidth);
  void tickWidgets(float deltaMs);
  bool onPointerEvent(const PointerEvent&);

private:
  float m_scale;
  bool m_dirty = true;
  std::function<void()> m_frame;
  std::vector<std::unique_ptr<Widget>> m_widgets;
};
