#pragma once
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "shell/settings/display_layout.h"
#include "ui/builders.h"
#include "ui/controls/label.h"

#include <linux/input-event-codes.h>

namespace settings {
  class DisplayLayoutCanvas final : public Node {
  public:
    DisplayLayoutCanvas(
        std::vector<DisplayRect> rectangles, std::vector<std::string> names, std::size_t selected, float scale,
        std::function<void(std::size_t, DisplayRect, bool)> commit
    )
        : m_rectangles(std::move(rectangles)), m_scale(scale), m_commit(std::move(commit)) {
      setSize(600 * scale, 250 * scale);
      setClipChildren(true);
      for (std::size_t i = 0; i < m_rectangles.size(); ++i) {
        auto box = std::make_unique<RectNode>();
        box->setStyle(
            {.fill = colorForRole(ColorRole::SurfaceVariant),
             .border = colorForRole(i == selected ? ColorRole::Primary : ColorRole::Outline),
             .fillMode = FillMode::Solid,
             .radius = 6 * scale,
             .softness = 1,
             .borderWidth = 2 * scale}
        );
        m_boxes.push_back(box.get());
        addChild(std::move(box));
        auto title = ui::label({.text = std::to_string(i + 1) + " · " + names[i], .fontSize = 14 * scale});
        m_labels.push_back(title.get());
        addChild(std::move(title));
      }
      auto area = std::make_unique<InputArea>();
      m_area = area.get();
      area->setOnPress([this](const InputArea::PointerData& d) {
        if (d.button != BTN_LEFT)
          return;
        if (d.pressed) {
          for (std::size_t i = m_boxes.size(); i-- > 0;) {
            auto* box = m_boxes[i];
            if (d.localX >= box->x()
                && d.localX <= box->x() + box->width()
                && d.localY >= box->y()
                && d.localY <= box->y() + box->height()) {
              m_drag = int(i);
              m_startX = d.localX;
              m_startY = d.localY;
              m_original = m_rectangles[i];
              m_moved = false;
              break;
            }
          }
        } else if (m_drag >= 0) {
          const auto i = std::size_t(m_drag);
          m_drag = -1;
          // Reject overlapping drops. A monitor must occupy its own part of the desktop.
          if (m_moved && std::ranges::any_of(m_rectangles, [&](const auto& rect) {
                return &rect != &m_rectangles[i] && overlaps(m_rectangles[i], rect);
              })) {
            m_rectangles[i] = m_original;
            m_moved = false;
            redraw();
          }
          m_commit(i, m_rectangles[i], m_moved);
        }
      });
      area->setOnMotion([this](const InputArea::PointerData& d) {
        if (m_drag < 0 || !m_area->pressed())
          return;
        if (std::hypot(d.localX - m_startX, d.localY - m_startY) < 3 * m_scale && !m_moved)
          return;
        m_moved = true;
        auto rect = m_original;
        rect.x += (d.localX - m_startX) / m_zoom;
        rect.y += (d.localY - m_startY) / m_zoom;
        auto others = m_rectangles;
        others.erase(others.begin() + m_drag);
        m_rectangles[std::size_t(m_drag)] = snapDisplay(rect, others, 10 * m_scale / m_zoom);
        redraw();
      });
      area->setOnCancel([this] {
        if (m_drag >= 0)
          m_rectangles[std::size_t(m_drag)] = m_original;
        m_drag = -1;
        m_moved = false;
        redraw();
      });
      addChild(std::move(area));
      fit();
      redraw();
    }

  private:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Node::doArrange(renderer, rect);
      if (m_drag < 0)
        fit();
      redraw();
    }
    void fit() {
      float left = m_rectangles[0].x, top = m_rectangles[0].y, right = left, bottom = top;
      for (auto r : m_rectangles) {
        left = std::min(left, r.x);
        top = std::min(top, r.y);
        right = std::max(right, r.x + r.width);
        bottom = std::max(bottom, r.y + r.height);
      }
      m_zoom = std::min(
          (width() - 100 * m_scale) / std::max(1.F, right - left),
          (height() - 70 * m_scale) / std::max(1.F, bottom - top)
      );
      m_offsetX = (width() - (right - left) * m_zoom) / 2 - left * m_zoom;
      m_offsetY = (height() - (bottom - top) * m_zoom) / 2 - top * m_zoom;
    }
    void redraw() {
      for (std::size_t i = 0; i < m_rectangles.size(); ++i) {
        auto r = m_rectangles[i];
        auto* box = m_boxes[i];
        box->setPosition(m_offsetX + r.x * m_zoom, m_offsetY + r.y * m_zoom);
        box->setSize(r.width * m_zoom, r.height * m_zoom);
        m_labels[i]->setPosition(box->x() + 8 * m_scale, box->y() + 8 * m_scale);
        m_labels[i]->setMaxWidth(std::max(1.F, box->width() - 16 * m_scale));
      }
      m_area->setSize(width(), height());
    }
    std::vector<DisplayRect> m_rectangles;
    std::vector<RectNode*> m_boxes;
    std::vector<Label*> m_labels;
    InputArea* m_area = nullptr;
    float m_scale, m_zoom = 1, m_offsetX = 0, m_offsetY = 0, m_startX = 0, m_startY = 0;
    int m_drag = -1;
    bool m_moved = false;
    DisplayRect m_original{};
    std::function<void(std::size_t, DisplayRect, bool)> m_commit;
  };
} // namespace settings
