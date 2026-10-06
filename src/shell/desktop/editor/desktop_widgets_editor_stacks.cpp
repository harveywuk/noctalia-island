#include "i18n/i18n.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/desktop/widget_transform.h"
#include "ui/builders.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace {
  std::pair<float, float> cardExtent(const DesktopWidgetState& state, float scale) {
    auto size = desktop_cards::Size::Medium;
    if (const auto it = state.settings.find("card_size"); it != state.settings.end())
      if (const auto* value = std::get_if<std::string>(&it->second))
        size = desktop_cards::sizeFromSetting(*value);
    return {
        state.boxWidth > 0
            ? state.boxWidth
            : (size == desktop_cards::Size::Small ? desktop_cards::kSmallExtent : desktop_cards::kLargeExtent) * scale,
        state.boxHeight > 0
            ? state.boxHeight
            : (size == desktop_cards::Size::Large ? desktop_cards::kLargeExtent : desktop_cards::kSmallExtent) * scale
    };
  }
} // namespace

void DesktopWidgetsEditor::buildStackDropPreview(OverlaySurface& surface, Node& root) {
  auto preview = ui::box(
      {.fill = colorSpecFromRole(ColorRole::Primary, .18F),
       .border = colorSpecFromRole(ColorRole::Primary),
       .borderWidth = 2.0F,
       .radius = Style::scaledRadiusXl()}
  );
  preview->setHitTestVisible(false);
  preview->setVisible(false);
  preview->setZIndex(190);
  surface.stackDropPreview = preview.get();
  auto caption = ui::box({.fill = colorSpecFromRole(ColorRole::Surface, .96F), .radius = Style::scaledRadiusMd()});
  caption->addChild(
      ui::label(
          {.out = &surface.stackDropLabel,
           .fontWeight = FontWeight::Bold,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxLines = 2}
      )
  );
  preview->addChild(std::move(caption));
  root.addChild(std::move(preview));
}

void DesktopWidgetsEditor::hideStackDropPreviews() {
  for (auto& surface : m_surfaces) {
    if (surface->stackDropPreview) {
      surface->stackDropPreview->setVisible(false);
      surface->surface->requestRedraw();
    }
  }
}

void DesktopWidgetsEditor::updateStackTarget(OverlaySurface& surface, float pointerX, float pointerY) {
  hideStackDropPreviews();
  m_drag.stackTargetId.clear();
  m_drag.moved = m_drag.moved
      || std::hypot(m_currentEventSceneX - m_drag.startSceneX, m_currentEventSceneY - m_drag.startSceneY)
          > 12 * widgetContentScale();
  if (!m_drag.moved
      || m_altHeld
      || m_selectedWidgetIds.size() != 1
      || !m_drag.groupInitialStates.empty()
      || (surface.toolbar && surface.toolbar->containsScenePoint(pointerX, pointerY))
      || (surface.historyToolbar && surface.historyToolbar->containsScenePoint(pointerX, pointerY))
      || (surface.inspector && surface.inspector->containsScenePoint(pointerX, pointerY)))
    return;
  for (const auto& target : m_snapshot.widgets | std::views::reverse) {
    const auto view = surface.views.find(target.id);
    if (view == surface.views.end() || !desktop_stacks::canJoin(m_snapshot.widgets, m_drag.widgetId, target.id))
      continue;
    float x = 0, y = 0;
    if (!Node::mapFromScene(view->second.transformNode, pointerX, pointerY, x, y))
      continue;
    const auto width = view->second.intrinsicWidth, height = view->second.intrinsicHeight;
    // The middle half is intentional: brushing an edge keeps normal placement and snapping.
    if (x < width * .25F || x > width * .75F || y < height * .25F || y > height * .75F)
      continue;
    m_drag.stackTargetId = target.id;
    const auto bounds = computeWidgetTransformBounds(target.cx, target.cy, width, height, 1, target.rotationRad);
    auto* preview = surface.stackDropPreview;
    preview->setZIndex(190);
    preview->setPosition(bounds.left, bounds.top);
    preview->setFrameSize(bounds.aabbWidth, bounds.aabbHeight);
    preview->setVisible(true);
    auto* label = surface.stackDropLabel;
    label->setVisible(true);
    label->parent()->setVisible(true);
    label->setText(
        i18n::tr(target.type == "stack" ? "desktop-widgets.stack.drop-add" : "desktop-widgets.stack.drop-create")
    );
    label->setMaxWidth(std::max(1.0F, bounds.aabbWidth - 40));
    label->measure(surface.surface->renderTarget().renderer());
    label->setPosition(8, 6);
    static_cast<Box*>(label->parent())->setFrameSize(label->width() + 16, label->height() + 12);
    label->parent()->setPosition(
        (bounds.aabbWidth - label->width() - 16) * .5F, (bounds.aabbHeight - label->height() - 12) * .5F
    );
    surface.surface->requestRedraw();
    return;
  }
}

void DesktopWidgetsEditor::startStackMemberDrag(
    const std::string& stack, const std::string& member, const std::string& output
) {
  const auto* state = findWidgetState(member);
  if (!state)
    return;
  m_drag = {};
  m_drag.mode = DragMode::StackMember;
  m_drag.sourceStackId = stack;
  m_drag.widgetId = member;
  m_drag.initialState = *state;
  m_drag.startSceneX = m_currentEventSceneX;
  m_drag.startSceneY = m_currentEventSceneY;
  m_drag.surfaceOutputName = m_drag.moveSourceOutputName = output;
  const auto [width, height] = cardExtent(*state, widgetContentScale());
  m_drag.intrinsicWidth = width;
  m_drag.intrinsicHeight = height;
}

void DesktopWidgetsEditor::updateStackMemberDrag() {
  hideStackDropPreviews();
  m_drag.stackInsertion.reset();
  m_drag.memberDropOutside = false;
  m_drag.moved = m_drag.moved
      || std::hypot(m_currentEventSceneX - m_drag.startSceneX, m_currentEventSceneY - m_drag.startSceneY)
          > 8 * widgetContentScale();
  if (!m_drag.moved)
    return;
  float x = m_currentEventSceneX, y = m_currentEventSceneY;
  m_drag.surfaceOutputName = m_drag.moveSourceOutputName;
  if (const auto* source = desktop_widgets::findOutputByKey(*m_wayland, m_drag.moveSourceOutputName)) {
    const double globalX = static_cast<double>(source->logicalX) + x;
    const double globalY = static_cast<double>(source->logicalY) + y;
    for (const auto& output : m_wayland->outputs()) {
      if (!output.done || !output.output || !output.hasUsableGeometry())
        continue;
      if (globalX >= output.logicalX
          && globalY >= output.logicalY
          && globalX < output.logicalX + output.effectiveLogicalWidth()
          && globalY < output.logicalY + output.effectiveLogicalHeight()) {
        m_drag.surfaceOutputName = desktop_widgets::outputKey(output);
        x = static_cast<float>(globalX - output.logicalX);
        y = static_cast<float>(globalY - output.logicalY);
        break;
      }
    }
  }
  auto* surface = findSurface(m_drag.surfaceOutputName);
  if (!surface || !surface->stackDropPreview)
    return;
  auto* preview = surface->stackDropPreview;
  preview->setZIndex(250);
  auto* label = surface->stackDropLabel;
  label->setVisible(false);
  label->parent()->setVisible(false);
  if (surface->inspector && surface->inspector->containsScenePoint(x, y)) {
    for (std::size_t i = 0; i < surface->stackMemberRows.size(); ++i) {
      auto* row = surface->stackMemberRows[i].second;
      if (!row->containsScenePoint(x, y))
        continue;
      m_drag.stackInsertion = i;
      float left = 0, top = 0;
      Node::mapToScene(row, 0, 0, left, top);
      preview->setPosition(left, top);
      preview->setFrameSize(row->width(), row->height());
      preview->setVisible(true);
      break;
    }
  } else if (
      (!surface->toolbar || !surface->toolbar->containsScenePoint(x, y))
      && (!surface->historyToolbar || !surface->historyToolbar->containsScenePoint(x, y))
      && x >= 0
      && y >= 0
      && x < static_cast<float>(surface->surface->width())
      && y < static_cast<float>(surface->surface->height())
  ) {
    m_drag.memberDropOutside = true;
    auto placed = m_drag.initialState;
    placed.outputName = surface->outputName;
    placed.cx = x;
    placed.cy = y;
    if (shouldSnap()) {
      const auto grid = static_cast<float>(std::max(1, m_snapshot.grid.cellSize));
      placed.cx = std::round(x / grid) * grid;
      placed.cy = std::round(y / grid) * grid;
    }
    desktop_widgets::clampStateToOutput(*m_wayland, placed, m_drag.intrinsicWidth, m_drag.intrinsicHeight);
    m_drag.dropX = placed.cx;
    m_drag.dropY = placed.cy;
    preview->setPosition(placed.cx - m_drag.intrinsicWidth * .5F, placed.cy - m_drag.intrinsicHeight * .5F);
    preview->setFrameSize(m_drag.intrinsicWidth, m_drag.intrinsicHeight);
    preview->setVisible(true);
    label->setVisible(true);
    label->parent()->setVisible(true);
    label->setText(
        i18n::tr("desktop-widgets.stack.drop-remove", "name", desktop_settings::desktopWidgetTypeLabel(placed.type))
    );
    label->setMaxWidth(std::max(1.0F, m_drag.intrinsicWidth - 40));
    label->measure(surface->surface->renderTarget().renderer());
    label->setPosition(8, 6);
    static_cast<Box*>(label->parent())->setFrameSize(label->width() + 16, label->height() + 12);
    label->parent()->setPosition(
        (m_drag.intrinsicWidth - label->width() - 16) * .5F, (m_drag.intrinsicHeight - label->height() - 12) * .5F
    );
  }
  surface->surface->requestRedraw();
}

void DesktopWidgetsEditor::finishStackDrop() {
  const auto drag = std::exchange(m_drag, {});
  if (drag.mode == DragMode::StackMember) {
    const auto previousMembers = desktop_stacks::cards(m_snapshot.widgets, drag.sourceStackId);
    if (drag.moved && drag.stackInsertion)
      desktop_stacks::reorder(m_snapshot.widgets, drag.sourceStackId, drag.widgetId, *drag.stackInsertion);
    else if (
        drag.moved
        && drag.memberDropOutside
        && desktop_stacks::detach(
            m_snapshot.widgets, drag.sourceStackId, drag.widgetId, drag.surfaceOutputName, drag.dropX, drag.dropY
        )
    ) {
      setSingleSelection(drag.widgetId);
      m_inspectorOpen = false;
      // The last card takes the old stack's position, retaining its own size and settings.
      for (const auto& member : previousMembers) {
        if (member.id != drag.widgetId && findWidgetState(drag.sourceStackId))
          continue;
        if (auto* state = findWidgetState(member.id)) {
          const auto [width, height] = cardExtent(*state, widgetContentScale());
          desktop_widgets::clampStateToOutput(*m_wayland, *state, width, height);
        }
      }
    }
  } else {
    // Dropping onto a stack must not replace the source card's saved standalone position.
    const auto* source = findWidgetState(drag.widgetId);
    const auto moved = source ? *source : DesktopWidgetState{};
    if (auto* state = findWidgetState(drag.widgetId))
      *state = drag.initialState;
    const auto stack = desktop_stacks::join(m_snapshot.widgets, drag.widgetId, drag.stackTargetId, nextWidgetId());
    if (!stack.empty()) {
      setSingleSelection(stack);
    } else if (auto* state = findWidgetState(drag.widgetId)) {
      *state = moved;
    }
  }
  requestLayout();
}

void DesktopWidgetsEditor::cancelStackDrag() {
  const auto drag = std::exchange(m_drag, {});
  if (drag.mode == DragMode::Move || drag.mode == DragMode::Scale || drag.mode == DragMode::Rotate) {
    if (auto* state = findWidgetState(drag.widgetId))
      *state = drag.initialState;
    for (const auto& [id, initial] : drag.groupInitialStates)
      if (auto* state = findWidgetState(id))
        *state = initial.state;
  }
  hideStackDropPreviews();
  hideSnapGuides();
  for (auto& surface : m_surfaces)
    surface->inputDispatcher.cancelPointerCapture();
  requestLayout();
}
