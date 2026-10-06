#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widget_placement.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/desktop/widget_transform.h"
#include "ui/builders.h"
#include "ui/node_motion.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

void DesktopWidgetsEditor::startGalleryDrag(const std::string& output) {
  auto* surface = findSurface(output);
  if (!surface || !surface->galleryPreview || m_galleryWidgetType.empty())
    return;
  m_drag = {};
  m_drag.mode = DragMode::Gallery;
  m_drag.startSceneX = m_currentEventSceneX;
  m_drag.startSceneY = m_currentEventSceneY;
  m_drag.moveSourceOutputName = m_drag.surfaceOutputName = output;
  m_drag.initialState.id = nextWidgetId();
  m_drag.initialState.type = m_galleryWidgetType;
  m_drag.initialState.outputName = output;
  m_drag.initialState.settings = desktop_settings::newDesktopWidgetSettings(m_galleryWidgetType, m_galleryCardSize);
  m_drag.intrinsicWidth = std::max(32.0F, surface->galleryPreview->intrinsicWidth());
  m_drag.intrinsicHeight = std::max(32.0F, surface->galleryPreview->intrinsicHeight());
  m_history.breakGroup();
}

void DesktopWidgetsEditor::updateGalleryDrag() {
  if (m_drag.mode != DragMode::Gallery || !m_wayland)
    return;
  for (auto& surface : m_surfaces)
    if (surface->galleryDragNode)
      surface->galleryDragNode->setVisible(false);
  hideStackDropPreviews();
  hideSnapGuides();
  m_drag.memberDropOutside = false;
  m_drag.stackTargetId.clear();
  m_drag.moved = m_drag.moved
      || m_currentEventOutputName != m_drag.moveSourceOutputName
      || std::hypot(m_currentEventSceneX - m_drag.startSceneX, m_currentEventSceneY - m_drag.startSceneY)
          > 8 * widgetContentScale();
  if (!m_drag.moved)
    return;
  for (auto& surface : m_surfaces)
    if (surface->galleryOverlay) {
      surface->galleryOverlay->setVisible(false);
      surface->surface->requestRedraw();
    }

  const auto* source = desktop_widgets::findOutputByKey(*m_wayland, m_currentEventOutputName);
  if (!source)
    return;
  const double globalX = source->logicalX + static_cast<double>(m_currentEventSceneX);
  const double globalY = source->logicalY + static_cast<double>(m_currentEventSceneY);
  const WaylandOutput* destination = nullptr;
  for (const auto& output : m_wayland->outputs()) {
    if (output.done
        && output.output
        && output.hasUsableGeometry()
        && globalX >= output.logicalX
        && globalY >= output.logicalY
        && globalX < output.logicalX + output.effectiveLogicalWidth()
        && globalY < output.logicalY + output.effectiveLogicalHeight()) {
      destination = &output;
      break;
    }
  }
  if (!destination)
    return;
  auto* surface = findSurface(desktop_widgets::outputKey(*destination));
  if (!surface || !surface->stackDropPreview)
    return;
  const float x = static_cast<float>(globalX - destination->logicalX);
  const float y = static_cast<float>(globalY - destination->logicalY);
  if ((surface->toolbar && surface->toolbar->containsScenePoint(x, y))
      || (surface->historyToolbar && surface->historyToolbar->containsScenePoint(x, y))
      || (surface->inspector && surface->inspector->containsScenePoint(x, y)))
    return;
  m_drag.surfaceOutputName = surface->outputName;
  m_drag.memberDropOutside = true;
  auto draft = m_drag.initialState;
  draft.outputName = surface->outputName;
  draft.cx = x;
  draft.cy = y;
  auto candidates = m_snapshot.widgets;
  candidates.push_back(draft);
  std::vector<desktop_placement::Rect> neighbours;
  WidgetTransformBounds bounds;
  for (const auto& target : m_snapshot.widgets | std::views::reverse) {
    const auto view = surface->views.find(target.id);
    if (view == surface->views.end() || !target.enabled)
      continue;
    const auto targetBounds = computeWidgetTransformBounds(
        target.cx, target.cy, view->second.intrinsicWidth, view->second.intrinsicHeight, 1, target.rotationRad
    );
    neighbours.push_back({targetBounds.left, targetBounds.top, targetBounds.aabbWidth, targetBounds.aabbHeight});
    if (m_altHeld || !m_drag.stackTargetId.empty() || !desktop_stacks::canJoin(candidates, draft.id, target.id))
      continue;
    float localX = 0, localY = 0;
    if (!Node::mapFromScene(view->second.transformNode, x, y, localX, localY))
      continue;
    const float width = view->second.intrinsicWidth, height = view->second.intrinsicHeight;
    if (localX >= width * .25F && localX <= width * .75F && localY >= height * .25F && localY <= height * .75F) {
      m_drag.stackTargetId = target.id;
      bounds = targetBounds;
    }
  }
  if (m_drag.stackTargetId.empty()) {
    if (shouldSnap()) {
      const auto snapped = desktop_placement::snap(
          {x - m_drag.intrinsicWidth * .5F, y - m_drag.intrinsicHeight * .5F, m_drag.intrinsicWidth,
           m_drag.intrinsicHeight},
          neighbours, static_cast<float>(surface->surface->width()), static_cast<float>(surface->surface->height()),
          static_cast<float>(m_snapshot.grid.cellSize), Style::spaceLg * widgetContentScale(), 8 * widgetContentScale()
      );
      draft.cx += snapped.x.offset;
      draft.cy += snapped.y.offset;
      if (snapped.x.guide && surface->snapGuideX) {
        surface->snapGuideX->setPosition(*snapped.x.guide, 0);
        surface->snapGuideX->setVisible(true);
      }
      if (snapped.y.guide && surface->snapGuideY) {
        surface->snapGuideY->setPosition(0, *snapped.y.guide);
        surface->snapGuideY->setVisible(true);
      }
    }
    const auto clamped = clampWidgetCenterToOutput(
        draft.cx, draft.cy, m_drag.intrinsicWidth, m_drag.intrinsicHeight, 1, 0,
        static_cast<float>(surface->surface->width()), static_cast<float>(surface->surface->height())
    );
    draft.cx = clamped.cx;
    draft.cy = clamped.cy;
    bounds = computeWidgetTransformBounds(draft.cx, draft.cy, m_drag.intrinsicWidth, m_drag.intrinsicHeight, 1, 0);
  }
  m_drag.dropX = draft.cx;
  m_drag.dropY = draft.cy;
  // Carry an inert copy of the real card while its outline shows the snapped destination.
  // Each output owns its copy so its textures never cross renderer contexts.
  if (!surface->galleryDragWidget && m_factory && m_renderContext) {
    m_renderContext->makeCurrent(surface->surface->renderTarget());
    auto& renderer = surface->surface->renderTarget().renderer();
    surface->galleryDragWidget = m_factory->create(draft.type, draft.settings, widgetContentScale());
    if (auto* widget = surface->galleryDragWidget.get()) {
      widget->create();
      widget->setEditorPreview(true);
      widget->update(renderer);
      widget->layout(renderer);
      auto ghost = ui::node({.width = m_drag.intrinsicWidth, .height = m_drag.intrinsicHeight});
      ghost->setHitTestVisible(false);
      ghost->setExcludeSubtreeFromTabOrder(true);
      ghost->setZIndex(240);
      ghost->addChild(widget->releaseRoot());
      surface->galleryDragNode = surface->sceneRoot->addChild(std::move(ghost));
      Motion::liftNode(*surface->galleryDragNode, true);
    }
  }
  if (auto* ghost = surface->galleryDragNode) {
    ghost->setVisible(true);
    ghost->setPosition(x - m_drag.intrinsicWidth * .5F, y - m_drag.intrinsicHeight * .5F);
    ghost->setOpacity(m_drag.stackTargetId.empty() ? .8F : .45F);
  }
  auto* preview = surface->stackDropPreview;
  preview->setZIndex(250);
  preview->setPosition(bounds.left, bounds.top);
  preview->setFrameSize(bounds.aabbWidth, bounds.aabbHeight);
  preview->setVisible(true);
  auto* label = surface->stackDropLabel;
  label->setVisible(true);
  label->parent()->setVisible(true);
  const auto* target = findWidgetState(m_drag.stackTargetId);
  label->setText(
      target
          ? i18n::tr(target->type == "stack" ? "desktop-widgets.stack.drop-add" : "desktop-widgets.stack.drop-create")
          : i18n::tr("desktop-widgets.stack.drop-remove", "name", desktop_settings::desktopWidgetTypeLabel(draft.type))
  );
  label->setMaxWidth(std::max(1.0F, bounds.aabbWidth - 32));
  label->measure(surface->surface->renderTarget().renderer());
  label->setPosition(8, 6);
  static_cast<Box*>(label->parent())->setFrameSize(label->width() + 16, label->height() + 12);
  label->parent()->setPosition(
      (bounds.aabbWidth - label->width() - 16) * .5F, std::max(0.0F, bounds.aabbHeight - label->height() - 20)
  );
  surface->surface->requestRedraw();
}

void DesktopWidgetsEditor::cancelGalleryDrag() {
  m_drag = {};
  hideSnapGuides();
  hideStackDropPreviews();
  for (auto& surface : m_surfaces) {
    if (surface->galleryDragNode) {
      (void)surface->sceneRoot->removeChild(surface->galleryDragNode);
      surface->galleryDragNode = nullptr;
    }
    surface->galleryDragWidget.reset();
    surface->inputDispatcher.cancelPointerCapture();
    if (surface->galleryOverlay)
      surface->galleryOverlay->setVisible(true);
    surface->surface->requestRedraw();
  }
}

void DesktopWidgetsEditor::finishGalleryDrag() {
  const auto drag = m_drag;
  cancelGalleryDrag();
  if (!drag.moved || !drag.memberDropOutside)
    return;
  deferEditorMutation([this, drag]() {
    if (!findSurface(drag.surfaceOutputName))
      return;
    const auto position = std::pair{drag.dropX, drag.dropY};
    const auto setting = drag.initialState.settings.find("card_size");
    const auto* preset =
        setting == drag.initialState.settings.end() ? nullptr : std::get_if<std::string>(&setting->second);
    const std::string size = preset ? *preset : "";
    if (desktop_setup::guided(drag.initialState.type)) {
      m_galleryOutputName = drag.surfaceOutputName;
      startSetup(drag.surfaceOutputName, drag.initialState.type, size);
      m_setupPosition = position;
      m_setupStackTarget = drag.stackTargetId;
    } else {
      addWidget(drag.surfaceOutputName, drag.initialState.type, size, position);
      if (!drag.stackTargetId.empty()) {
        const auto stack =
            desktop_stacks::join(m_snapshot.widgets, m_selectedWidgetId, drag.stackTargetId, nextWidgetId());
        if (!stack.empty())
          setSingleSelection(stack);
      }
      closeGallery();
    }
  });
}
