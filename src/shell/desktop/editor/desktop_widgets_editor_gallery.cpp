#include "i18n/i18n.h"
#include "render/backend/render_backend.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <array>

void DesktopWidgetsEditor::closeGallery() {
  m_galleryOutputName.clear();
  requestLayout();
}

void DesktopWidgetsEditor::buildGallery(OverlaySurface& surface, Node& root) {
  Renderer& renderer = surface.surface->renderTarget().renderer();
  const auto options = desktop_settings::desktopWidgetTypeOptions();
  if (options.empty() || m_factory == nullptr) {
    return;
  }
  if (std::none_of(options.begin(), options.end(), [this](const auto& option) {
        return option.value == m_galleryWidgetType;
      })) {
    m_galleryWidgetType = options.front().value;
  }

  // The gallery is modal on its output. Its previews never receive pointer or keyboard input.
  for (const auto& child : root.children()) {
    child->setExcludeSubtreeFromTabOrder(true);
  }
  auto dismiss = [this]() { deferEditorMutation([this]() { closeGallery(); }); };
  auto overlay = ui::inputArea({
      .frameWidth = root.width(),
      .frameHeight = root.height(),
      .zIndex = 300,
      .onClick = [dismiss](const InputArea::PointerData&) { dismiss(); },
  });
  overlay->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Shadow, 0.3F),
          .width = root.width(),
          .height = root.height(),
          .configure = [](Box& box) { box.setHitTestVisible(false); },
      })
  );

  const float margin = Style::spaceLg;
  const float width = std::min(800.0F, root.width() - 2.0F * margin);
  const float height = std::min(640.0F, root.height() - 2.0F * margin);
  const bool compact = width < 600.0F;
  const float sidebarWidth = compact ? 0.0F : 196.0F;
  const float contentX = margin + sidebarWidth;
  const float contentWidth = width - contentX - margin;
  const bool cards = desktop_cards::supportsSizePresets(m_galleryWidgetType);

  auto panel = ui::inputArea({.frameWidth = width, .frameHeight = height, .clipChildren = true});
  panel->setPosition(std::round((root.width() - width) * 0.5F), std::round((root.height() - height) * 0.5F));
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
          .text = i18n::tr("desktop-widgets.editor.gallery.title"),
          .fontSize = Style::fontSizeHeader,
          .fontWeight = FontWeight::Bold,
      }),
      margin, margin + Style::spaceXs
  );
  place(
      ui::button({
          .glyph = "close",
          .controlHeight = Style::controlHeightSm,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("desktop-widgets.editor.gallery.close"),
          .onClick = dismiss,
      }),
      width - margin - Style::controlHeightSm, margin
  );

  auto chooseType = [this](std::string type) {
    deferEditorMutation([this, type = std::move(type)]() {
      m_galleryWidgetType = type;
      m_galleryCardSize = (type == "calendar" || type == "media_player") ? "medium" : "small";
      requestLayout();
    });
  };
  if (compact) {
    std::vector<std::string> labels;
    std::size_t selected = 0;
    for (std::size_t i = 0; i < options.size(); ++i) {
      labels.push_back(options[i].label);
      if (options[i].value == m_galleryWidgetType)
        selected = i;
    }
    place(
        ui::select({
            .options = std::move(labels),
            .selectedIndex = selected,
            .onSelectionChanged =
                [options, chooseType](std::size_t index, std::string_view) {
                  if (index < options.size())
                    chooseType(options[index].value);
                },
            .configure =
                [contentWidth](Select& select) {
                  select.setMinWidth(contentWidth);
                  select.setMaxWidth(contentWidth);
                },
        }),
        contentX, 60.0F
    );
  } else {
    auto list = ui::scrollView({
        .state = &m_galleryScroll,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .width = sidebarWidth - margin,
        .height = height - 76.0F,
    });
    list->content()->setGap(Style::spaceXs);
    for (const auto& option : options) {
      list->content()->addChild(
          ui::button({
              .text = option.label,
              .controlHeight = Style::controlHeight,
              .selected = option.value == m_galleryWidgetType,
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .width = sidebarWidth - margin - Style::spaceLg,
              .onClick = [chooseType, type = option.value]() { chooseType(type); },
          })
      );
    }
    place(std::move(list), margin, 60.0F);
    place(
        ui::label({
            .text = desktop_settings::desktopWidgetTypeLabel(m_galleryWidgetType),
            .fontSize = Style::fontSizeTitle,
            .fontWeight = FontWeight::Bold,
            .maxWidth = contentWidth,
        }),
        contentX, 64.0F
    );
  }

  if (cards) {
    auto sizes = ui::row({.gap = Style::spaceSm});
    for (const auto size : std::array{"small", "medium", "large"}) {
      sizes->addChild(
          ui::button({
              .text = i18n::tr(std::string("desktop-widgets.editor.settings.card-size-") + size),
              .controlHeight = Style::controlHeightSm,
              .selected = m_galleryCardSize == size,
              .variant = ButtonVariant::Default,
              .onClick = [this, size]() {
                deferEditorMutation([this, size]() {
                  m_galleryCardSize = size;
                  requestLayout();
                });
              },
          })
      );
    }
    place(std::move(sizes), contentX, 104.0F);
  }

  const float previewY = cards ? 148.0F : 108.0F;
  const float previewHeight = std::max(32.0F, height - previewY - 92.0F);
  auto preview = ui::box({
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F),
      .radius = Style::scaledRadiusLg(),
      .width = contentWidth,
      .height = previewHeight,
      .configure = [](Box& box) {
        box.setClipChildren(true);
        box.setHitTestVisible(false);
        box.setExcludeSubtreeFromTabOrder(true);
      },
  });
  surface.galleryPreview = m_factory->create(
      m_galleryWidgetType, desktop_settings::newDesktopWidgetSettings(m_galleryWidgetType, m_galleryCardSize),
      widgetContentScale()
  );
  if (surface.galleryPreview != nullptr) {
    auto& widget = *surface.galleryPreview;
    widget.create();
    widget.setEditorPreview(true);
    widget.update(renderer);
    widget.layout(renderer);
    const float widgetWidth = std::max(1.0F, widget.intrinsicWidth());
    const float widgetHeight = std::max(1.0F, widget.intrinsicHeight());
    auto widgetRoot = widget.releaseRoot();
    if (widgetRoot != nullptr) {
      const float scale = std::max(
          0.01F,
          std::min({1.0F, (contentWidth - 2.0F * margin) / widgetWidth, (previewHeight - 2.0F * margin) / widgetHeight})
      );
      widgetRoot->setTransformOrigin(0.0F, 0.0F);
      widgetRoot->setScale(scale, scale);
      widgetRoot->setPosition(
          (contentWidth - widgetWidth * scale) * 0.5F, (previewHeight - widgetHeight * scale) * 0.5F
      );
      widgetRoot->setHitTestVisible(false);
      preview->addChild(std::move(widgetRoot));
    }
  }
  if (m_galleryWidgetType == "sticker") {
    auto placeholder = ui::glyph({.glyph = "photo", .glyphSize = 64.0F});
    placeholder->layout(renderer);
    placeholder->setPosition(
        (contentWidth - placeholder->width()) * 0.5F, (previewHeight - placeholder->height()) * 0.5F
    );
    preview->addChild(std::move(placeholder));
  }
  place(std::move(preview), contentX, previewY);
  place(
      ui::label({
          .text = i18n::tr(
              m_galleryWidgetType == "sticker" ? "desktop-widgets.editor.gallery.choose-image"
                                               : "desktop-widgets.editor.gallery.customize-hint"
          ),
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxWidth = contentWidth,
          .maxLines = 2,
      }),
      contentX, height - 80.0F
  );
  place(
      ui::button({
          .text = i18n::tr("desktop-widgets.editor.gallery.add"),
          .glyph = "plus",
          .controlHeight = Style::controlHeight,
          .variant = ButtonVariant::Primary,
          .onClick =
              [this, outputName = surface.outputName]() {
                deferEditorMutation([this, outputName]() {
                  addWidget(outputName, m_galleryWidgetType, m_galleryCardSize);
                  closeGallery();
                });
              },
      }),
      contentX, height - margin - Style::controlHeight
  );

  overlay->addChild(std::move(panel));
  overlay->layout(renderer);
  root.addChild(std::move(overlay));
}
