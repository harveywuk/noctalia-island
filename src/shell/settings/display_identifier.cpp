#include "shell/settings/display_identifier.h"

#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>

namespace settings {
  struct DisplayIdentifier::Instance {
    std::unique_ptr<Node> root;
    std::unique_ptr<LayerSurface> surface;
  };
  DisplayIdentifier::DisplayIdentifier(WaylandConnection& wayland, RenderContext& render)
      : m_wayland(wayland), m_render(render) {}
  DisplayIdentifier::~DisplayIdentifier() = default;
  void DisplayIdentifier::show(const std::vector<std::string>& names) {
    m_timeout.stop();
    m_instances.clear();
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto& outputs = m_wayland.outputs();
      const auto output = std::ranges::find(outputs, names[i], &WaylandOutput::connectorName);
      if (output == outputs.end() || !output->done || !output->output || !output->hasUsableGeometry())
        continue;
      auto inst = std::make_unique<Instance>();
      inst->surface = std::make_unique<LayerSurface>(
          m_wayland,
          LayerSurfaceConfig{
              .nameSpace = "noctalia-display-identify",
              .layer = LayerShellLayer::Overlay,
              .width = 260,
              .height = 150,
              .exclusiveZone = -1,
              .keyboard = LayerShellKeyboard::None,
              .defaultWidth = 260,
              .defaultHeight = 150
          }
      );
      auto* ptr = inst.get();
      inst->surface->setRenderContext(&m_render);
      inst->surface->setConfigureCallback([ptr](auto, auto) { ptr->surface->requestLayout(); });
      inst->surface->setPrepareFrameCallback([this, ptr, number = i + 1,
                                              name = names[i]](bool needsUpdate, bool needsLayout) {
        if (ptr->root) {
          if (needsUpdate || needsLayout) {
            m_render.makeCurrent(ptr->surface->renderTarget());
            UiPhaseScope phase(UiPhase::Layout);
            ptr->root->layout(ptr->surface->renderTarget().renderer());
          }
          return;
        }
        m_render.makeCurrent(ptr->surface->renderTarget());
        UiPhaseScope phase(UiPhase::Layout);
        ptr->root = std::make_unique<Node>();
        ptr->root->setSize(260, 150);
        auto background = std::make_unique<RectNode>();
        background->setSize(260, 150);
        background->setStyle(
            {.fill = colorForRole(ColorRole::Surface),
             .border = colorForRole(ColorRole::Primary),
             .fillMode = FillMode::Solid,
             .radius = 18,
             .softness = 1,
             .borderWidth = 2}
        );
        ptr->root->addChild(std::move(background));
        auto numberLabel = ui::label({.text = std::to_string(number), .fontSize = 60});
        numberLabel->setPosition(20, 12);
        ptr->root->addChild(std::move(numberLabel));
        auto connector = ui::label({.text = name, .fontSize = 20});
        connector->setPosition(20, 106);
        connector->setMaxWidth(220);
        ptr->root->addChild(std::move(connector));
        ptr->root->layout(ptr->surface->renderTarget().renderer());
        ptr->surface->setSceneRoot(ptr->root.get());
      });
      if (!inst->surface->initialize(output->output))
        continue;
      inst->surface->setClickThrough(true);
      inst->surface->setInputRegion({});
      m_instances.push_back(std::move(inst));
    }
    m_timeout.start(std::chrono::seconds(5), [this] { m_instances.clear(); });
  }
} // namespace settings
