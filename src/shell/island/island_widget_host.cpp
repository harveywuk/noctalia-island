#include "shell/island/island_widget_host.h"

#include "config/config_types.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_factory.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/box.h"
#include "ui/style.h"

#include <algorithm>
#include <string>
#include <utility>

IslandWidgetHost::IslandWidgetHost(
    WidgetFactory& factory, const Config& config, wl_output* output, float scale, AnimationManager* animations,
    const noctalia::bar::WidgetActionDispatcher* dispatcher, std::function<void()> update, std::function<void()> redraw,
    std::function<void()> frame
)
    : m_scale(scale), m_frame(std::move(frame)) {
  setClipChildren(true);
  BarConfig bar;
  bar.name = "island";
  bar.position = "top";
  bar.scale = scale;
  for (const auto& name : config.island.hoverWidgets) {
    const auto found = config.widgets.find(name);
    const WidgetConfig* wc = found == config.widgets.end() ? nullptr : &found->second;
    const std::string type = wc && !wc->type.empty() ? wc->type : name;
    const auto options = resolveCommonWidgetOptions(bar, wc, type, scale);
    if (!options.enabled)
      continue;
    auto widget = factory.create(name, output, options.contentScale, "top", "island", 8 * scale, options.enableScroll);
    if (!widget)
      continue;
    widget->setConfigName(name);
    widget->setWidgetForeground(options.color);
    widget->setWidgetIconColor(options.iconColor);
    widget->applyCommonOptions(options, FontWeight::Medium, config.shell.fontFamily, "island.hover_widgets");
    widget->setActionContext(
        IpcInvocationContext{.widgetName = name, .widgetType = type, .barName = "island", .output = output}
    );
    widget->resolveGestureBindings(type, wc, nullptr, "island", dispatcher);
    widget->setAnimationManager(animations);
    widget->setUpdateCallback([this, update] {
      m_dirty = true;
      update();
    });
    widget->setRedrawCallback(redraw);
    widget->setFrameTickRequestCallback(m_frame);
    widget->setPanelToggleCallback([output](
                                       std::string_view panel, std::string_view context, std::optional<float>,
                                       std::optional<float>, Widget::PanelActivation activation
                                   ) {
      PanelOpenRequest request{.output = output, .context = context};
      if (activation == Widget::PanelActivation::Open)
        PanelManager::instance().openPanel(std::string(panel), request);
      else
        PanelManager::instance().togglePanel(std::string(panel), request);
    });
    widget->create();
    if (auto root = widget->releaseRoot()) {
      auto cell = std::make_unique<Node>();
      cell->setClipChildren(true);
      cell->addChild(std::move(root));
      addChild(std::move(cell));
      m_widgets.push_back(std::move(widget));
    }
  }
}

IslandWidgetHost::~IslandWidgetHost() {
  // Widget destructors cancel timers and listeners while their scene nodes still exist.
  m_widgets.clear();
}

void IslandWidgetHost::updateWidgets(Renderer& renderer, float availableWidth) {
  const bool dirty = std::exchange(m_dirty, false);
  const float gap = 8 * m_scale;
  const float rowHeight = 36 * m_scale;
  float x = 0, y = 0, lineHeight = 0;
  for (std::size_t i = 0; i < m_widgets.size(); ++i) {
    auto& widget = *m_widgets[i];
    if (dirty)
      widget.update(renderer);
    widget.layout(renderer, availableWidth, rowHeight);
    auto& cell = *children()[i];
    const bool visible = widget.outerNode()
        && widget.outerNode()->visible()
        && widget.outerNode()->participatesInLayout()
        && widget.width() > 0;
    cell.setVisible(visible);
    if (!visible)
      continue;
    const float width = std::min(availableWidth, widget.width());
    const float height = std::max(rowHeight, widget.height());
    if (x > 0 && x + width > availableWidth) {
      x = 0;
      y += lineHeight + gap;
      lineHeight = 0;
    }
    cell.setPosition(x, y);
    cell.setSize(width, height);
    widget.outerNode()->setPosition(0, (height - widget.height()) / 2);
    x += width + gap;
    lineHeight = std::max(lineHeight, height);
  }
  setSize(availableWidth, y + lineHeight);
  if (std::ranges::any_of(m_widgets, [](const auto& widget) { return widget->needsFrameTick(); }))
    m_frame();
}

void IslandWidgetHost::tickWidgets(float deltaMs) {
  for (auto& widget : m_widgets)
    if (widget->needsFrameTick())
      widget->onFrameTick(deltaMs);
  if (std::ranges::any_of(m_widgets, [](const auto& widget) { return widget->needsFrameTick(); }))
    m_frame();
}

bool IslandWidgetHost::onPointerEvent(const PointerEvent& event) {
  for (auto& widget : m_widgets)
    if (widget->onPointerEvent(event))
      return true;
  return false;
}
