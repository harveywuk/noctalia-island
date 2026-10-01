#include "shell/settings/island_widget_editor.h"

#include "config/config_service.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <cmath>
#include <memory>

namespace settings {
  namespace {
    struct Lane {
      Flex* body = nullptr;
      Box* indicator = nullptr;
      std::vector<Flex*> cards;
    };
    struct Drag {
      bool active = false;
      bool moved = false;
      float startX = 0, startY = 0;
      std::optional<std::size_t> lane;
      std::size_t insertion = 0;
    };

    void hideIndicators(const std::array<Lane, 3>& lanes) {
      for (const auto& lane : lanes)
        if (lane.indicator)
          lane.indicator->setVisible(false);
    }

    bool withinClips(const Node* node, float x, float y) {
      for (auto* ancestor = node->parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->clipChildren())
          continue;
        float ax = 0, ay = 0;
        Node::absolutePosition(ancestor, ax, ay);
        if (x < ax || x > ax + ancestor->width() || y < ay || y > ay + ancestor->height())
          return false;
      }
      return true;
    }
  } // namespace

  void addIslandWidgetEditor(Flex& section, SettingsControlFactory& factory, const std::vector<std::string>& root) {
    const auto& ctx = factory.context();
    const float scale = ctx.scale;
    auto layout = hoverLayout(islandConfigForPath(ctx.config, root));
    layout.scope = root;
    auto block = ui::column({.align = FlexAlign::Stretch, .gap = 8 * scale});
    block->addChild(makeSettingSubtitleLabel(i18n::tr("settings.island-editor.description"), scale));
    auto presets = ui::row({.align = FlexAlign::Center, .gap = 6 * scale});
    constexpr std::array<std::string_view, 3> presetKeys{"minimal", "media", "system"};
    for (std::size_t i = 0; i < presetKeys.size(); ++i) {
      presets->addChild(
          ui::button({
              .text = i18n::tr("settings.island-editor.presets." + std::string(presetKeys[i])),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Default,
              .minHeight = 30 * scale,
              .onClick = [i, layout, root, undo = ctx.hoverLayoutUndo, commit = ctx.setOverrides] {
                if (undo)
                  *undo = std::make_shared<HoverLayout>(layout);
                commit(hoverLayoutOverrides(hoverLayoutPreset(i), root));
              },
          })
      );
    }
    if (ctx.hoverLayoutUndo && *ctx.hoverLayoutUndo && (*ctx.hoverLayoutUndo)->scope == root) {
      presets->addChild(
          ui::button({
              .text = i18n::tr("settings.island-editor.undo"),
              .glyph = "arrow-back-up",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = 30 * scale,
              .onClick = [undo = ctx.hoverLayoutUndo, commit = ctx.setOverrides] {
                auto previous = std::exchange(*undo, {});
                if (previous)
                  commit(hoverLayoutOverrides(*previous, previous->scope));
              },
          })
      );
    }
    block->addChild(std::move(presets));
    block->addChild(makeSettingSubtitleLabel(i18n::tr("settings.island-editor.preset-hint"), scale));

    const auto move = [groups = layout.groups, root, commit = ctx.setOverrides](
                          std::size_t from, std::size_t index, std::size_t to, std::size_t insertion
                      ) {
      auto changed = groups;
      if (!moveHoverWidget(changed, from, index, to, insertion))
        return;
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> updates;
      updates.push_back({islandPath(root, kHoverWidgetKeys[from]), changed[from]});
      if (from != to)
        updates.push_back({islandPath(root, kHoverWidgetKeys[to]), changed[to]});
      commit(std::move(updates));
    };
    auto lanes = std::make_shared<std::array<Lane, 3>>();
    auto columns = ui::row({.align = FlexAlign::Stretch, .gap = 8 * scale, .fillWidth = true});
    for (std::size_t group = 0; group < lanes->size(); ++group) {
      const auto path = islandPath(root, kHoverWidgetKeys[group]);
      auto lane = ui::column({
          .align = FlexAlign::Stretch,
          .gap = 6 * scale,
          .padding = 8 * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F),
          .radius = Style::scaledRadiusMd(scale),
          .minWidth = 160 * scale,
          .flexGrow = 1.0F,
      });
      (*lanes)[group].body = lane.get();
      auto header = ui::row({.align = FlexAlign::Center, .gap = 4 * scale});
      header->addChild(
          ui::label({
              .text = i18n::tr("settings.schema.island." + path.back() + ".label"),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Bold,
              .flexGrow = 1.0F,
          })
      );
      header->addChild(
          ui::button({
              .glyph = "plus",
              .tooltip = i18n::tr("settings.island-editor.add"),
              .minWidth = 26 * scale,
              .minHeight = 26 * scale,
              .padding = 2 * scale,
              .onClick = [open = ctx.openBarWidgetAddPopup, path] {
                if (open)
                  open(path);
              },
          })
      );
      lane->addChild(std::move(header));
      if (ctx.configService && ctx.configService->hasOverride(path))
        lane->addChild(factory.makeResetButton(path));
      auto indicator = ui::box({
          .out = &(*lanes)[group].indicator,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = 1.5F * scale,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(10); },
      });
      lane->addChild(std::move(indicator));
      const auto& names = layout.groups[group];
      if (names.empty()) {
        lane->addChild(
            ui::label({
                .text = i18n::tr("settings.island-editor.empty"),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 2,
            })
        );
        lane->addChild(ui::box({.fill = clearColorSpec(), .height = 40 * scale}));
      }
      for (std::size_t index = 0; index < names.size(); ++index) {
        const auto name = names[index];
        const auto info = widgetReferenceInfo(ctx.config, name, false);
        const auto type = widgetTypeForReference(ctx.config, name);
        auto card = ui::column({
            .align = FlexAlign::Stretch,
            .gap = 2 * scale,
            .padding = 3 * scale,
            .fill = colorSpecFromRole(ColorRole::Surface, 0.72F),
            .radius = Style::scaledRadiusSm(scale),
        });
        auto* cardPtr = card.get();
        (*lanes)[group].cards.push_back(cardPtr);
        auto title = ui::row({.align = FlexAlign::Center, .gap = 2 * scale});
        Button* handle = nullptr;
        auto handleNode = ui::button({
            .out = &handle,
            .glyph = "menu-2",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("settings.entities.widget.group.drag"),
            .minWidth = 24 * scale,
            .minHeight = 26 * scale,
            .padding = 2 * scale,
            .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
        });
        auto drag = std::make_shared<Drag>();
        const auto cancel = [drag, lanes, cardPtr] {
          drag->active = false;
          drag->lane.reset();
          cardPtr->setOpacity(1);
          hideIndicators(*lanes);
        };
        handle->inputArea()->setOnCancel(cancel);
        handle->setOnPress([drag, lanes, cardPtr, cancel, move, group, index](float x, float y, bool pressed) {
          if (pressed) {
            *drag = Drag{.active = true, .startX = x, .startY = y};
            cardPtr->setOpacity(0.65F);
          } else {
            const auto target = drag->lane;
            const auto insertion = drag->insertion;
            const bool commit = drag->active && drag->moved && target.has_value();
            cancel();
            if (commit)
              move(group, index, *target, insertion);
          }
        });
        handle->setOnPointerMotion([drag, lanes, handle, scale](float x, float y) {
          if (!drag->active)
            return;
          if (std::hypot(x - drag->startX, y - drag->startY) >= Style::dragStartThreshold * scale)
            drag->moved = true;
          if (!drag->moved)
            return;
          float ax = 0, ay = 0;
          Node::absolutePosition(handle, ax, ay);
          x += ax;
          y += ay;
          drag->lane.reset();
          hideIndicators(*lanes);
          for (std::size_t target = 0; target < lanes->size(); ++target) {
            const auto& targetLane = (*lanes)[target];
            if (!targetLane.body)
              continue;
            if (!withinClips(targetLane.body, x, y))
              continue;
            Node::absolutePosition(targetLane.body, ax, ay);
            if (x < ax || x > ax + targetLane.body->width() || y < ay || y > ay + targetLane.body->height())
              continue;
            std::size_t insertion = 0;
            float marker = 36 * scale;
            for (auto* item : targetLane.cards) {
              float ix = 0, iy = 0;
              Node::absolutePosition(item, ix, iy);
              if (y < iy + item->height() / 2) {
                marker = iy - ay - 3 * scale;
                break;
              }
              ++insertion;
              marker = iy - ay + item->height() + 3 * scale;
            }
            drag->lane = target;
            drag->insertion = insertion;
            targetLane.indicator->setPosition(6 * scale, marker);
            targetLane.indicator->setSize(std::max(1.0F, targetLane.body->width() - 12 * scale), 3 * scale);
            targetLane.indicator->setVisible(true);
            break;
          }
        });
        title->addChild(std::move(handleNode));
        title->addChild(
            ui::label({
                .text = info.title,
                .fontSize = Style::fontSizeCaption * scale,
                .maxLines = 1,
                .flexGrow = 1.0F,
            })
        );
        if (!type.empty()) {
          title->addChild(
              ui::button({
                  .glyph = "settings",
                  .variant = ButtonVariant::Ghost,
                  .tooltip = i18n::tr("settings.entities.widget.group.settings-widget"),
                  .minWidth = 24 * scale,
                  .minHeight = 26 * scale,
                  .padding = 2 * scale,
                  .onClick = [name, path, open = ctx.openWidgetInspectorEditor] {
                    if (open)
                      open(path, name);
                  },
              })
          );
        }
        title->addChild(
            ui::button({
                .glyph = "x",
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.entities.widget.group.remove-widget"),
                .minWidth = 24 * scale,
                .minHeight = 26 * scale,
                .padding = 2 * scale,
                .onClick = [names, index, path, commit = ctx.setOverride] {
                  auto updated = names;
                  updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(index));
                  commit(path, updated);
                },
            })
        );
        card->addChild(std::move(title));
        auto actions = ui::row({.align = FlexAlign::Center, .gap = 2 * scale});
        const auto arrow = [&](const char* glyph, const char* label, bool enabled, std::size_t target,
                               std::size_t insertion) {
          actions->addChild(
              ui::button({
                  .glyph = glyph,
                  .enabled = enabled,
                  .variant = ButtonVariant::Ghost,
                  .tooltip = i18n::tr(label),
                  .minWidth = 24 * scale,
                  .minHeight = 22 * scale,
                  .padding = 2 * scale,
                  .onClick = [move, group, index, target, insertion] { move(group, index, target, insertion); },
              })
          );
        };
        arrow("chevron-up", "settings.island-editor.up", index > 0, group, index ? index - 1 : 0);
        arrow("chevron-down", "settings.island-editor.down", index + 1 < names.size(), group, index + 2);
        arrow(
            "chevron-left", "settings.island-editor.left", group > 0, group ? group - 1 : 0,
            group ? layout.groups[group - 1].size() : 0
        );
        arrow(
            "chevron-right", "settings.island-editor.right", group < 2, std::min(group + 1, std::size_t{2}),
            group < 2 ? layout.groups[group + 1].size() : 0
        );
        card->addChild(std::move(actions));
        lane->addChild(std::move(card));
      }
      columns->addChild(std::move(lane));
    }
    block->addChild(std::move(columns));
    section.addChild(std::move(block));
  }
} // namespace settings
