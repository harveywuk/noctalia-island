#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/backend/render_backend.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

void DesktopWidgetsEditor::recordHistory(const std::string& group) {
  if (!m_open || m_drag.mode != DragMode::None)
    return;
  m_history.record(m_snapshot, group);
  for (auto& surface : m_surfaces) {
    if (surface->undoButton)
      surface->undoButton->setEnabled(m_history.canUndo());
    if (surface->redoButton)
      surface->redoButton->setEnabled(m_history.canRedo());
    if (surface->surface)
      surface->surface->requestRedraw();
  }
}

void DesktopWidgetsEditor::travelHistory(bool redo) {
  if (m_drag.mode != DragMode::None || m_setupDraft || !m_galleryOutputName.empty() || !m_layoutOutput.empty())
    return;
  const auto* snapshot = redo ? m_history.redo() : m_history.undo();
  if (!snapshot)
    return;
  // Invalidate pending inspector callbacks before replacing their widget states.
  ++m_setupRevision;
  m_snapshot = *snapshot;
  const auto membership = desktop_stacks::resolve(m_snapshot.widgets);
  std::erase_if(m_selectedWidgetIds, [&](const auto& id) {
    return !findWidgetState(id) || desktop_stacks::contains(membership, id);
  });
  if (!m_selectedWidgetIds.contains(m_selectedWidgetId))
    m_selectedWidgetId = m_selectedWidgetIds.empty() ? "" : *m_selectedWidgetIds.begin();
  for (auto& surface : m_surfaces) {
    surface->inputDispatcher.cancelPointerCapture();
    surface->inputDispatcher.setFocus(nullptr);
  }
  requestLayout();
}

void DesktopWidgetsEditor::positionHistoryToolbar(OverlaySurface& surface) {
  if (!surface.historyToolbar || !surface.toolbar || !surface.surface)
    return;
  const float x = std::clamp(
      surface.toolbarX, 0.0F,
      std::max(0.0F, static_cast<float>(surface.surface->width()) - surface.historyToolbar->width())
  );
  const float above = surface.toolbarY - surface.historyToolbar->height() - Style::spaceXs;
  const float below = surface.toolbarY + surface.toolbar->height() + Style::spaceXs;
  surface.historyToolbar->setPosition(x, above >= 0 ? above : below);
}

void DesktopWidgetsEditor::buildHistoryToolbar(OverlaySurface& surface, Node& root) {
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceXs,
      .paddingV = Style::spaceXs,
      .paddingH = Style::spaceSm,
      .fill = colorSpecFromRole(ColorRole::Surface, .94F),
      .radius = Style::scaledRadiusLg(),
      .configure = [](Flex& flex) {
        flex.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
        flex.setZIndex(200);
      },
  });
  row->addChild(
      ui::button({
          .out = &surface.undoButton,
          .text = i18n::tr("desktop-widgets.editor.history.undo"),
          .glyph = "arrow-back-up",
          .controlHeight = Style::controlHeightSm,
          .enabled = m_history.canUndo(),
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("desktop-widgets.editor.history.undo-shortcut"),
          .onClick = [this]() { deferEditorMutation([this]() { travelHistory(false); }); },
      })
  );
  row->addChild(
      ui::button({
          .out = &surface.redoButton,
          .text = i18n::tr("desktop-widgets.editor.history.redo"),
          .glyph = "arrow-forward-up",
          .controlHeight = Style::controlHeightSm,
          .enabled = m_history.canRedo(),
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("desktop-widgets.editor.history.redo-shortcut"),
          .onClick = [this]() { deferEditorMutation([this]() { travelHistory(true); }); },
      })
  );
  if (!m_profile.showLockscreenLoginPreview) {
    row->addChild(
        ui::button({
            .text = i18n::tr("desktop-widgets.editor.layouts.title"),
            .glyph = "layout-grid",
            .controlHeight = Style::controlHeightSm,
            .variant = ButtonVariant::Ghost,
            .onClick = [this, output = surface.outputName]() {
              deferEditorMutation([this, output]() { openLayouts(output); });
            },
        })
    );
  }
  row->layout(surface.surface->renderTarget().renderer());
  surface.historyToolbar = row.get();
  root.addChild(std::move(row));
  positionHistoryToolbar(surface);
}

void DesktopWidgetsEditor::loadLayouts() {
  m_layouts.clear();
  m_layoutsReadable = true;
  m_layoutError.clear();
  if (m_profile.showLockscreenLoginPreview || !m_config)
    return;
  if (const auto data = m_config->stateString("desktop_layouts", "presets")) {
    if (const auto decoded = desktop_layouts::decode(*data))
      m_layouts = *decoded;
    else {
      m_layoutsReadable = false;
      m_layoutError = "desktop-widgets.editor.layouts.unreadable";
    }
  }
}

void DesktopWidgetsEditor::openLayouts(const std::string& output) {
  closeSetup();
  m_galleryOutputName.clear();
  m_inspectorOpen = false;
  m_layoutOutput = output;
  m_layoutName.clear();
  m_layoutScroll = {};
  loadLayouts();
  requestLayout();
}

void DesktopWidgetsEditor::closeLayouts() {
  for (auto& surface : m_surfaces)
    surface->inputDispatcher.setFocus(nullptr);
  ++m_setupRevision;
  m_layoutOutput.clear();
  requestLayout();
}

bool DesktopWidgetsEditor::storeLayouts(std::vector<desktop_layouts::Layout> layouts) {
  if (!m_layoutsReadable || !m_config)
    return false;
  const auto encoded = desktop_layouts::encode(layouts);
  // Never save data that this version cannot read back, or overwrite unsupported data.
  const bool valid = desktop_layouts::decode(encoded).has_value();
  if (!valid || !m_config->setStateString("desktop_layouts", "presets", encoded)) {
    if (valid) {
      // StateStore updates memory before writing. Restore it as well on failure.
      (void)m_config->setStateString("desktop_layouts", "presets", desktop_layouts::encode(m_layouts));
    }
    m_layoutError = "desktop-widgets.editor.layouts.save-failed";
    requestLayout();
    return false;
  }
  m_layouts = std::move(layouts);
  m_layoutError.clear();
  requestLayout();
  return true;
}

void DesktopWidgetsEditor::saveLayout(std::optional<std::size_t> replace) {
  if (m_layoutOutput.empty() || (replace && *replace >= m_layouts.size()))
    return;
  const auto name = replace ? m_layouts[*replace].name : desktop_layouts::cleanName(m_layoutName);
  if (name.empty()) {
    m_layoutError = "desktop-widgets.editor.layouts.invalid-name";
    requestLayout();
    return;
  }
  if (!replace && std::ranges::any_of(m_layouts, [&](const auto& layout) { return layout.name == name; })) {
    m_layoutError = "desktop-widgets.editor.layouts.duplicate-name";
    requestLayout();
    return;
  }
  if (!replace && m_layouts.size() >= desktop_layouts::maxLayouts)
    return;
  auto snapshot = m_snapshot;
  desktop_widgets::PlacementMapper{}.rebaseForCurrentOutputs(*m_wayland, snapshot.widgets);
  const bool monitorOnly = replace ? !m_layouts[*replace].output.empty() : m_layoutMonitorOnly;
  auto layout = desktop_layouts::capture(snapshot, name, monitorOnly ? m_layoutOutput : "", [this](const auto& widget) {
    return effectiveOutputName(widget);
  });
  auto layouts = m_layouts;
  if (replace)
    layouts[*replace] = std::move(layout);
  else
    layouts.push_back(std::move(layout));
  if (storeLayouts(std::move(layouts)))
    m_layoutName.clear();
}

void DesktopWidgetsEditor::applyLayout(std::size_t index) {
  if (index >= m_layouts.size() || m_layoutOutput.empty())
    return;
  m_history.breakGroup();
  auto layout = m_layouts[index];
  if (!layout.output.empty()) {
    for (auto& widget : layout.snapshot.widgets)
      widget.outputName = m_layoutOutput;
  }
  (void)desktop_widgets::PlacementMapper{}.remapForOutputChange(*m_wayland, layout.snapshot.widgets);
  m_snapshot = desktop_layouts::apply(m_snapshot, layout, m_layoutOutput, [this](const auto& widget) {
    return effectiveOutputName(widget);
  });
  clearSelection();
  closeLayouts();
}

void DesktopWidgetsEditor::deleteLayout(std::size_t index) {
  if (index >= m_layouts.size())
    return;
  auto layouts = m_layouts;
  layouts.erase(layouts.begin() + static_cast<std::ptrdiff_t>(index));
  storeLayouts(std::move(layouts));
}

void DesktopWidgetsEditor::buildLayouts(OverlaySurface& surface, Node& root) {
  Renderer& renderer = surface.surface->renderTarget().renderer();
  for (const auto& child : root.children())
    child->setExcludeSubtreeFromTabOrder(true);
  const auto dismiss = [this]() { deferEditorMutation([this]() { closeLayouts(); }); };
  auto overlay = ui::inputArea({
      .frameWidth = root.width(),
      .frameHeight = root.height(),
      .zIndex = 300,
      .onClick = [dismiss](const InputArea::PointerData&) { dismiss(); },
  });
  overlay->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Shadow, .3F),
          .width = root.width(),
          .height = root.height(),
          .configure = [](Box& box) { box.setHitTestVisible(false); },
      })
  );
  const float margin = Style::spaceLg;
  const float width = std::min(680.0F, root.width() - margin * 2);
  const float height = std::min(580.0F, root.height() - margin * 2);
  const float contentWidth = width - margin * 2;
  auto panel = ui::inputArea({.frameWidth = width, .frameHeight = height, .clipChildren = true});
  panel->setPosition(std::round((root.width() - width) * .5F), std::round((root.height() - height) * .5F));
  panel->addChild(
      ui::box({
          .width = width,
          .height = height,
          .configure = [](Box& box) {
            box.setDialogStyle();
            box.setHitTestVisible(false);
          },
      })
  );
  auto place = [&](std::unique_ptr<Node> node, float x, float y) {
    node->layout(renderer);
    node->setPosition(Style::rtl() ? width - x - node->width() : x, y);
    panel->addChild(std::move(node));
  };
  place(
      ui::label({
          .text = i18n::tr("desktop-widgets.editor.layouts.title"),
          .fontSize = Style::fontSizeHeader,
          .fontWeight = FontWeight::Bold,
      }),
      margin, margin
  );
  place(
      ui::button({
          .glyph = "close",
          .controlHeight = Style::controlHeightSm,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("desktop-widgets.editor.layouts.close"),
          .onClick = dismiss,
      }),
      width - margin - Style::controlHeightSm, margin
  );
  place(
      ui::label({
          .text = i18n::tr("desktop-widgets.editor.layouts.hint"),
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxWidth = contentWidth,
          .maxLines = 2,
      }),
      margin, 64
  );

  auto list = ui::scrollView({
      .state = &m_layoutScroll,
      .viewportPaddingH = 0,
      .viewportPaddingV = 0,
      .width = contentWidth,
      .height = std::max(40.0F, height - 304.0F),
  });
  list->content()->setGap(Style::spaceSm);
  if (m_layouts.empty()) {
    list->content()->addChild(
        ui::label({
            .text = i18n::tr("desktop-widgets.editor.layouts.empty"),
            .fontSize = Style::fontSizeBody,
            .maxWidth = contentWidth - Style::spaceMd,
        })
    );
  }
  for (std::size_t i = 0; i < m_layouts.size(); ++i) {
    const auto& layout = m_layouts[i];
    const std::string scope = layout.output.empty()
        ? i18n::tr("desktop-widgets.editor.layouts.desktop")
        : i18n::tr("desktop-widgets.editor.layouts.monitor") + " · " + layout.output;
    list->content()->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm,
                .paddingV = Style::spaceSm,
                .paddingH = Style::spaceSm,
                .fill = colorSpecFromRole(ColorRole::SurfaceVariant, .5F),
                .radius = Style::scaledRadiusLg(),
                .width = contentWidth - Style::spaceMd,
            },
            ui::column(
                {.align = FlexAlign::Start, .gap = Style::spaceXs, .flexGrow = 1.0F},
                ui::label(
                    {.text = layout.name,
                     .fontSize = Style::fontSizeBody,
                     .fontWeight = FontWeight::Bold,
                     .maxWidth = std::max(60.0F, contentWidth - 240)}
                ),
                ui::label(
                    {.text = scope,
                     .fontSize = Style::fontSizeCaption,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                     .maxWidth = std::max(60.0F, contentWidth - 240)}
                )
            ),
            ui::button({
                .text = i18n::tr("desktop-widgets.editor.layouts.apply"),
                .controlHeight = Style::controlHeightSm,
                .variant = ButtonVariant::Secondary,
                .tooltip = i18n::tr(
                    layout.output.empty() ? "desktop-widgets.editor.layouts.apply-desktop"
                                          : "desktop-widgets.editor.layouts.apply-monitor"
                ),
                .onClick = [this, i]() { deferEditorMutation([this, i]() { applyLayout(i); }); },
            }),
            ui::button({
                .glyph = "refresh",
                .controlHeight = Style::controlHeightSm,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("desktop-widgets.editor.layouts.update"),
                .onClick = [this, i]() { deferEditorMutation([this, i]() { saveLayout(i); }); },
            }),
            ui::button({
                .glyph = "trash",
                .controlHeight = Style::controlHeightSm,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("desktop-widgets.editor.layouts.delete"),
                .onClick = [this, i]() { deferEditorMutation([this, i]() { deleteLayout(i); }); },
            })
        )
    );
  }
  place(std::move(list), margin, 100);
  const float formY = height - 184;
  place(
      ui::input({
          .value = m_layoutName,
          .placeholder = i18n::tr("desktop-widgets.editor.layouts.name"),
          .enabled = m_layoutsReadable,
          .width = contentWidth,
          .onChange = [this](const auto& value) { m_layoutName = value; },
          .onSubmit = [this](const auto&) { deferEditorMutation([this]() { saveLayout(); }); },
      }),
      margin, formY
  );
  place(
      ui::select({
          .options =
              std::vector<std::string>{
                  i18n::tr("desktop-widgets.editor.layouts.desktop"),
                  i18n::tr("desktop-widgets.editor.layouts.monitor") + " · " + surface.outputName
              },
          .selectedIndex = m_layoutMonitorOnly ? 1UL : 0UL,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                deferEditorMutation([this, index]() {
                  m_layoutMonitorOnly = index == 1;
                  requestLayout();
                });
              },
          .configure =
              [contentWidth](Select& select) {
                select.setMinWidth(contentWidth);
                select.setMaxWidth(contentWidth);
              },
      }),
      margin, formY + 44
  );
  const auto error = !m_layoutError.empty()             ? m_layoutError
      : m_layouts.size() >= desktop_layouts::maxLayouts ? "desktop-widgets.editor.layouts.limit"
                                                        : "";
  if (!error.empty()) {
    place(
        ui::label({
            .text = i18n::tr(error),
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::Error),
            .maxWidth = std::max(40.0F, contentWidth - 152),
            .maxLines = 3,
        }),
        margin, height - 74
    );
  }
  place(
      ui::button({
          .text = i18n::tr("desktop-widgets.editor.layouts.save"),
          .glyph = "device-floppy",
          .enabled = m_layoutsReadable && m_layouts.size() < desktop_layouts::maxLayouts,
          .variant = ButtonVariant::Primary,
          .width = 140,
          .onClick = [this]() { deferEditorMutation([this]() { saveLayout(); }); },
      }),
      width - margin - 140, height - 64
  );
  overlay->addChild(std::move(panel));
  root.addChild(std::move(overlay));
}
