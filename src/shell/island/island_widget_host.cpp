#include "shell/island/island_widget_host.h"

#include "config/config_types.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_factory.h"
#include "shell/island/island_widget_layout.h"
#include "shell/panel/panel_manager.h"

#include <algorithm>
#include <string>
#include <utility>

IslandWidgetHost::IslandWidgetHost(
    WidgetFactory& factory, const Config& config, wl_output* output, float scale, AnimationManager* animations,
    const noctalia::bar::WidgetActionDispatcher* dispatcher, std::function<void()> update, std::function<void()> redraw,
    std::function<void()> frame, const IslandConfig& island, const std::string& barName
)
    : m_scale(scale), m_frame(std::move(frame)) {
  setClipChildren(true);
  BarConfig bar;
  bar.name = barName;
  bar.position = "top";
  bar.scale = scale;
  const std::array lists{&island.hoverWidgets, &island.hoverWidgetsCenter, &island.hoverWidgetsRight};
  for (std::size_t group = 0; group < lists.size(); ++group) {
    for (const auto& name : *lists[group]) {
      const auto found = config.widgets.find(name);
      const WidgetConfig* wc = found == config.widgets.end() ? nullptr : &found->second;
      const std::string type = wc && !wc->type.empty() ? wc->type : name;
      const auto options = resolveCommonWidgetOptions(bar, wc, type, scale);
      if (!options.enabled)
        continue;
      auto widget = factory.create(name, output, options.contentScale, "top", barName, 8 * scale, options.enableScroll);
      if (!widget)
        continue;
      widget->setConfigName(name);
      // The Cupertino Island is black in both themes; unstyled widgets draw white on it.
      const bool cupertino = island.appearance == IslandAppearance::Cupertino;
      const auto white = fixedColorSpec(rgba(1.0F, 1.0F, 1.0F));
      widget->setWidgetForeground(options.color || !cupertino ? options.color : std::optional<ColorSpec>(white));
      widget->setWidgetIconColor(options.iconColor || !cupertino ? options.iconColor : std::optional<ColorSpec>(white));
      widget->applyCommonOptions(options, FontWeight::Medium, config.shell.fontFamily, "island.hover_widgets");
      widget->setActionContext(
          IpcInvocationContext{.widgetName = name, .widgetType = type, .barName = barName, .output = output}
      );
      widget->resolveGestureBindings(type, wc, nullptr, barName, dispatcher);
      widget->setAnimationManager(animations);
      widget->setUpdateCallback([this, update] {
        m_dirty = true;
        update();
      });
      widget->setRedrawCallback(redraw);
      widget->setFrameTickRequestCallback(m_frame);
      widget->setPanelToggleCallback([output, barName](
                                         std::string_view panel, std::string_view context, std::optional<float>,
                                         std::optional<float>, Widget::PanelActivation activation
                                     ) {
        PanelOpenRequest request{.output = output, .context = context, .sourceBarName = barName};
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
        m_groups.push_back(group);
      }
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
  std::vector<island::HoverWidgetSize> sizes(m_widgets.size());
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
    sizes[i] = {widget.width(), std::max(rowHeight, widget.height()), m_groups[i]};
  }
  const auto layout = island::layoutHoverWidgets(sizes, availableWidth, gap);
  for (std::size_t i = 0; i < m_widgets.size(); ++i) {
    const auto& rect = layout.items[i];
    auto& cell = *children()[i];
    cell.setPosition(rect.x, rect.y);
    cell.setSize(rect.width, rect.height);
    if (auto* outer = m_widgets[i]->outerNode())
      outer->setPosition(0, (rect.height - m_widgets[i]->height()) / 2);
  }
  setSize(availableWidth, layout.height);
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
