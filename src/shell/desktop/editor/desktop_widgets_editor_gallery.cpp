#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/backend/render_backend.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/node_motion.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>

void DesktopWidgetsEditor::openGallery(const std::string& output) {
  m_galleryRevealPending = true;
  m_galleryClosing = false;
  m_galleryOutputName = output;
  m_galleryFocusSearch = true;
  m_gallerySaveFailed = false;
  m_inspectorOpen = false;
  m_galleryFavorites.clear();
  if (m_config)
    for (const auto& option : desktop_settings::desktopWidgetTypeOptions())
      if (m_config->stateBool("desktop_gallery", desktop_gallery::favoriteKey(option.value)).value_or(false))
        m_galleryFavorites.insert(option.value);
  requestLayout();
}

void DesktopWidgetsEditor::closeGallery(bool animated) {
  auto* surface = findSurface(m_galleryOutputName);
  if (animated && surface && surface->galleryOverlay && MotionService::instance().enabled()) {
    if (m_galleryClosing)
      return;
    m_galleryClosing = true;
    surface->inputDispatcher.cancelPointerCapture();
    surface->inputDispatcher.setFocus(nullptr);
    auto* overlay = surface->galleryOverlay;
    overlay->setExcludeSubtreeFromTabOrder(true);
    surface->animations.cancelForOwner(overlay);
    Motion::fadeNode(*overlay, 0, Motion::dismissMs, [this]() { deferEditorMutation([this]() { closeGallery(); }); });
    surface->surface->requestRedraw();
    return;
  }
  m_galleryClosing = false;
  m_galleryRevealPending = false;
  m_galleryOutputName.clear();
  m_galleryFocusSearch = false;
  requestLayout();
}

void DesktopWidgetsEditor::toggleGalleryFavorite(const std::string& type) {
  const bool favorite = m_galleryFavorites.contains(type);
  m_gallerySaveFailed =
      !m_config || !m_config->setStateBool("desktop_gallery", desktop_gallery::favoriteKey(type), !favorite);
  if (m_gallerySaveFailed) {
    if (m_config)
      (void)m_config->setStateBool("desktop_gallery", desktop_gallery::favoriteKey(type), favorite);
  } else if (favorite) {
    m_galleryFavorites.erase(type);
  } else {
    m_galleryFavorites.insert(type);
  }
  requestLayout();
}

void DesktopWidgetsEditor::addGallerySelection(const std::string& output) {
  if (m_galleryWidgetType.empty())
    return;
  if (desktop_setup::guided(m_galleryWidgetType)) {
    startSetup(output, m_galleryWidgetType, m_galleryCardSize);
  } else {
    addWidget(output, m_galleryWidgetType, m_galleryCardSize);
    closeGallery();
  }
}

void DesktopWidgetsEditor::buildGallery(OverlaySurface& surface, Node& root, std::unique_ptr<Node> search) {
  Renderer& renderer = surface.surface->renderTarget().renderer();
  auto options = desktop_settings::desktopWidgetTypeOptions();
  if (m_factory == nullptr)
    return;
  std::erase_if(options, [this](const auto& option) {
    const auto category = desktop_gallery::category(option.value);
    const auto label = desktop_gallery::filters[static_cast<std::size_t>(category)].label;
    return !desktop_gallery::matches(
        option.value, option.label, i18n::tr(label), m_galleryQuery, m_galleryCategory,
        m_galleryFavorites.contains(option.value)
    );
  });
  if (std::none_of(options.begin(), options.end(), [this](const auto& option) {
        return option.value == m_galleryWidgetType;
      })) {
    m_galleryWidgetType = options.empty() ? "" : options.front().value;
  }

  // Widget controls stay inert in previews. The preview frame owns placement gestures.
  for (const auto& child : root.children())
    child->setExcludeSubtreeFromTabOrder(true);
  auto dismiss = [this]() { deferEditorMutation([this]() { closeGallery(true); }); };
  auto overlay = ui::inputArea({
      .frameWidth = root.width(),
      .frameHeight = root.height(),
      .zIndex = 300,
      .onClick = [dismiss](const InputArea::PointerData&) { dismiss(); },
  });
  surface.galleryOverlay = overlay.get();
  overlay->setVisible(m_drag.mode != DragMode::Gallery || !m_drag.moved);
  overlay->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Shadow, 0.3F),
          .width = root.width(),
          .height = root.height(),
          .configure = [](Box& box) { box.setHitTestVisible(false); },
      })
  );

  const float margin = Style::spaceLg;
  const float width = std::min(920.0F, root.width() - 2.0F * margin);
  const float height = std::min(700.0F, root.height() - 2.0F * margin);
  const bool compact = width < 640.0F;
  const float sidebarWidth = compact ? 0.0F : 224.0F;
  const float contentX = margin + sidebarWidth;
  const float contentWidth = width - contentX - margin;
  const bool cards = desktop_cards::supportsSizePresets(m_galleryWidgetType);
  const float titleY = compact ? 156.0F : 112.0F;

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

  if (!search) {
    search = ui::input({
        .out = &surface.gallerySearch,
        .value = m_galleryQuery,
        .placeholder = i18n::tr("desktop-widgets.editor.gallery.search"),
        .clearButtonEnabled = true,
        .onChange = [this](const std::string& value) {
          m_galleryQuery = value;
          deferEditorMutation([this]() {
            m_galleryScroll = {};
            requestLayout();
          });
        },
    });
  }
  surface.gallerySearch = static_cast<Input*>(search.get());
  if (surface.gallerySearch->value() != m_galleryQuery)
    surface.gallerySearch->setValue(m_galleryQuery);
  search->setSize(width - 2 * margin, Style::controlHeight);
  place(std::move(search), margin, 64);

  std::vector<std::string> categories;
  for (const auto& filter : desktop_gallery::filters)
    categories.push_back(i18n::tr(filter.label));
  const float categoryWidth = compact ? contentWidth : sidebarWidth - margin;
  place(
      ui::select({
          .options = std::move(categories),
          .selectedIndex = static_cast<std::size_t>(m_galleryCategory),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                deferEditorMutation([this, index]() {
                  if (index < desktop_gallery::filters.size())
                    m_galleryCategory = desktop_gallery::filters[index].category;
                  m_galleryScroll = {};
                  requestLayout();
                });
              },
          .configure =
              [categoryWidth](Select& select) {
                select.setMinWidth(categoryWidth);
                select.setMaxWidth(categoryWidth);
              },
      }),
      margin, 112
  );

  auto chooseType = [this](std::string type) {
    deferEditorMutation([this, type = std::move(type)]() {
      m_galleryWidgetType = type;
      m_galleryCardSize = (type == "calendar" || type == "media_player") ? "medium" : "small";
      requestLayout();
    });
  };
  if (!compact) {
    auto list = ui::scrollView({
        .state = &m_galleryScroll,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .width = sidebarWidth - margin,
        .height = std::max(32.0F, height - 160.0F - margin),
    });
    list->content()->setGap(Style::spaceXs);
    for (const auto& option : options) {
      list->content()->addChild(
          ui::button({
              .text = option.label,
              .glyph = m_galleryFavorites.contains(option.value) ? "star-filled"
                                                                 : std::string(desktop_gallery::glyph(option.value)),
              .controlHeight = Style::controlHeight,
              .selected = option.value == m_galleryWidgetType,
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .width = sidebarWidth - margin - Style::spaceLg,
              .onClick = [chooseType, type = option.value]() { chooseType(type); },
          })
      );
    }
    place(std::move(list), margin, 160);
  }

  if (options.empty()) {
    place(
        ui::label({
            .text = i18n::tr(
                m_galleryCategory == desktop_gallery::Category::Favorites && m_galleryQuery.empty()
                    ? "desktop-widgets.editor.gallery.no-favorites"
                    : "desktop-widgets.editor.gallery.no-results"
            ),
            .fontSize = Style::fontSizeBody,
            .maxWidth = contentWidth,
            .maxLines = 3,
        }),
        contentX, titleY + 60
    );
    place(
        ui::button({
            .text = i18n::tr("desktop-widgets.editor.gallery.clear-filters"),
            .variant = ButtonVariant::Secondary,
            .onClick =
                [this]() {
                  deferEditorMutation([this]() {
                    m_galleryQuery.clear();
                    m_galleryCategory = desktop_gallery::Category::All;
                    m_galleryFocusSearch = true;
                    m_galleryScroll = {};
                    requestLayout();
                  });
                },
        }),
        contentX, titleY + 140
    );
  } else {
    const float nameWidth = contentWidth - Style::controlHeight - margin;
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
                  [nameWidth](Select& select) {
                    select.setMinWidth(nameWidth);
                    select.setMaxWidth(nameWidth);
                  },
          }),
          contentX, titleY
      );
    } else {
      place(
          ui::label({
              .text = desktop_settings::desktopWidgetTypeLabel(m_galleryWidgetType),
              .fontSize = Style::fontSizeTitle,
              .fontWeight = FontWeight::Bold,
              .maxWidth = nameWidth,
          }),
          contentX, titleY + Style::spaceXs
      );
    }
    const bool favorite = m_galleryFavorites.contains(m_galleryWidgetType);
    place(
        ui::button({
            .glyph = favorite ? "star-filled" : "star",
            .controlHeight = Style::controlHeight,
            .selected = favorite,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(
                favorite ? "desktop-widgets.editor.gallery.unfavorite" : "desktop-widgets.editor.gallery.favorite"
            ),
            .onClick =
                [this, type = m_galleryWidgetType]() {
                  deferEditorMutation([this, type]() { toggleGalleryFavorite(type); });
                },
        }),
        width - margin - Style::controlHeight, titleY
    );

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
      place(std::move(sizes), contentX, titleY + 44);
    }
    const float previewY = titleY + (cards ? 88.0F : 44.0F);
    const float previewHeight = std::max(32.0F, height - previewY - 108.0F);
    auto preview = ui::inputArea({
        .frameWidth = contentWidth,
        .frameHeight = previewHeight,
        .onPress = [this, output = surface.outputName](const InputArea::PointerData& data) {
          if (data.button == BTN_LEFT && data.pressed)
            startGalleryDrag(output);
        },
    });
    preview->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F),
            .radius = Style::scaledRadiusLg(),
            .width = contentWidth,
            .height = previewHeight,
            .configure = [](Box& box) { box.setHitTestVisible(false); },
        })
    );
    preview->setClipChildren(true);
    surface.galleryPreview = m_factory->create(
        m_galleryWidgetType, desktop_settings::newDesktopWidgetSettings(m_galleryWidgetType, m_galleryCardSize),
        widgetContentScale()
    );
    if (surface.galleryPreview) {
      auto& widget = *surface.galleryPreview;
      widget.create();
      widget.setEditorPreview(true);
      widget.update(renderer);
      widget.layout(renderer);
      const float widgetWidth = std::max(1.0F, widget.intrinsicWidth());
      const float widgetHeight = std::max(1.0F, widget.intrinsicHeight());
      auto widgetRoot = widget.releaseRoot();
      if (widgetRoot) {
        const float scale = std::max(
            0.01F,
            std::min({1.0F, (contentWidth - 2 * margin) / widgetWidth, (previewHeight - 2 * margin) / widgetHeight})
        );
        widgetRoot->setTransformOrigin(0, 0);
        widgetRoot->setScale(scale, scale);
        widgetRoot->setPosition(
            (contentWidth - widgetWidth * scale) * .5F, (previewHeight - widgetHeight * scale) * .5F
        );
        widgetRoot->setHitTestVisible(false);
        widgetRoot->setExcludeSubtreeFromTabOrder(true);
        preview->addChild(std::move(widgetRoot));
      }
    }
    if (m_galleryWidgetType == "sticker") {
      auto placeholder = ui::glyph({.glyph = "photo", .glyphSize = 64.0F});
      placeholder->layout(renderer);
      placeholder->setPosition(
          (contentWidth - placeholder->width()) * .5F, (previewHeight - placeholder->height()) * .5F
      );
      placeholder->setHitTestVisible(false);
      preview->addChild(std::move(placeholder));
    }
    place(std::move(preview), contentX, previewY);
    place(
        ui::label({
            .text = i18n::tr(
                m_gallerySaveFailed                    ? "desktop-widgets.editor.gallery.save-failed"
                    : m_galleryWidgetType == "sticker" ? "desktop-widgets.editor.gallery.choose-image"
                                                       : "desktop-widgets.editor.gallery.drag-hint"
            ),
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(m_gallerySaveFailed ? ColorRole::Error : ColorRole::OnSurfaceVariant),
            .maxWidth = contentWidth,
            .maxLines = 2,
        }),
        contentX, height - 94
    );
    place(
        ui::button({
            .text = i18n::tr(
                desktop_setup::guided(m_galleryWidgetType) ? "desktop-widgets.setup.start"
                                                           : "desktop-widgets.editor.gallery.add"
            ),
            .glyph = "plus",
            .controlHeight = Style::controlHeight,
            .variant = ButtonVariant::Primary,
            .onClick =
                [this, output = surface.outputName]() {
                  deferEditorMutation([this, output]() { addGallerySelection(output); });
                },
        }),
        contentX, height - margin - Style::controlHeight
    );
  }
  auto* card = panel.get();
  overlay->addChild(std::move(panel));
  overlay->layout(renderer);
  auto* backdrop = root.addChild(std::move(overlay));
  if (m_galleryRevealPending) {
    m_galleryRevealPending = false;
    Motion::revealNode(*card);
    backdrop->setOpacity(0);
    Motion::fadeNode(*backdrop, 1, Motion::contentMs);
  }
}
