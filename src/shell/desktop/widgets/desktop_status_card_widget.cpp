#include "shell/desktop/widgets/desktop_status_card_widget.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {
  std::string durationText(std::chrono::seconds duration) {
    const auto minutes = std::max<std::int64_t>(0, duration.count()) / 60;
    return minutes >= 60 ? std::format("{}h {}m", minutes / 60, minutes % 60) : std::format("{}m", minutes);
  }
} // namespace

DesktopStatusCardWidget::DesktopStatusCardWidget(
    Kind kind, DesktopWidgetRuntimeServices services, desktop_cards::Size size
)
    : m_kind(kind), m_services(services), m_size(size) {}

void DesktopStatusCardWidget::create() {
  auto node = ui::node({});
  node->addChild(
      ui::label(
          {.out = &m_heading,
           .fontWeight = FontWeight::Medium,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1}
      )
  );
  node->addChild(ui::label({.out = &m_valueLabel, .color = colorSpecFromRole(ColorRole::Primary), .maxLines = 1}));
  node->addChild(
      ui::label({.out = &m_detailLabel, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 2})
  );
  node->addChild(
      ui::glyph(
          {.out = &m_symbol,
           .glyph = m_kind == Kind::Batteries ? "battery-4" : "clock",
           .color = colorSpecFromRole(ColorRole::Primary)}
      )
  );
  for (auto& row : m_rows) {
    node->addChild(ui::label({.out = &row.title, .maxLines = 1}));
    node->addChild(
        ui::label({.out = &row.detail, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
    );
    node->addChild(ui::glyph({.out = &row.glyph, .glyph = "battery-4"}));
    node->addChild(ui::progressBar({.out = &row.bar}));
  }
  for (auto& bar : m_bars)
    node->addChild(ui::box({.out = &bar, .fill = colorSpecFromRole(ColorRole::Primary), .radius = 2.0F}));
  setRoot(std::move(node));
  refresh();
}

bool DesktopStatusCardWidget::refresh() {
  std::vector<Item> items;
  std::string value;
  std::string detail;
  std::array<float, 24> history{};
  if (m_kind == Kind::Batteries) {
    if (m_services.upower != nullptr) {
      auto devices = m_services.upower->batteryDevices();
      std::stable_sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        return a.isLaptopBattery() > b.isLaptopBattery();
      });
      for (const auto& device : devices) {
        if (!device.isPresent || !std::isfinite(device.state.percentage))
          continue;
        const float percentage = std::clamp(static_cast<float>(device.state.percentage), 0.0F, 100.0F);
        items.push_back(
            {device.isLaptopBattery() ? i18n::tr("desktop-widgets.cards.this-device") : device.model,
             std::format("{:.0f}%", percentage),
             device.isLaptopBattery() ? "device-laptop" : batteryDeviceGlyphName(device.type), percentage / 100.0F}
        );
        if (items.back().title.empty())
          items.back().title = i18n::tr("desktop-widgets.cards.battery");
        if (items.size() == 1 && device.state.state == BatteryState::Charging)
          detail = i18n::tr("desktop-widgets.cards.charging");
      }
    }
    if (m_services.bluetooth != nullptr) {
      for (const auto& device : m_services.bluetooth->devices()) {
        if (!device.connected || !device.hasBattery || device.batteryFromUPower)
          continue;
        items.push_back(
            {device.alias, std::format("{}%", device.batteryPercent), "bluetooth", device.batteryPercent / 100.0F}
        );
      }
    }
    value = items.empty() ? "" : items.front().detail;
    if (detail.empty())
      detail = items.empty() ? i18n::tr("desktop-widgets.cards.no-batteries") : items.front().title;
  } else if (m_services.screenTime != nullptr && m_services.screenTime->enabled()) {
    const auto snapshot = m_services.screenTime->snapshot(1);
    value = durationText(snapshot.total);
    detail = i18n::tr("desktop-widgets.cards.today");
    for (const auto& app : snapshot.apps) {
      if (app.total.count() <= 0)
        continue;
      items.push_back(
          {app.displayName, durationText(app.total), "app-window",
           static_cast<float>(app.total.count())
               / static_cast<float>(std::max<std::int64_t>(1, snapshot.total.count()))}
      );
    }
    auto maxSeconds = std::chrono::seconds{1};
    for (const auto bucket : snapshot.buckets)
      maxSeconds = std::max(maxSeconds, bucket);
    for (std::size_t i = 0; i < std::min(history.size(), snapshot.buckets.size()); ++i)
      history[i] = static_cast<float>(snapshot.buckets[i].count()) / static_cast<float>(maxSeconds.count());
  } else {
    detail = i18n::tr("desktop-widgets.cards.screen-time-disabled");
  }
  const bool changed = value != m_value || detail != m_detail || items != m_items || history != m_history;
  m_value = std::move(value);
  m_detail = std::move(detail);
  m_items = std::move(items);
  m_history = history;
  return changed;
}

void DesktopStatusCardWidget::doUpdate(Renderer&) {
  if (refresh() && !isLayingOut())
    requestLayout();
}

void DesktopStatusCardWidget::doLayout(Renderer& renderer) {
  refresh();
  const auto card =
      desktop_cards::resolve(m_size, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  const bool small = card.size == desktop_cards::Size::Small;
  const bool large = card.size == desktop_cards::Size::Large;
  const bool activity = m_kind == Kind::ScreenTime;
  const float scale = card.scale;
  auto place = [&](Node* node, float x, float y) {
    node->setPosition(Style::rtl() ? card.width - x - node->width() : x, y);
  };
  auto label = [&](Label* node, std::string text, float x, float y, float width, float size) {
    node->setText(std::move(text));
    node->setFontSize(size * scale);
    node->setMinWidth(width);
    node->setMaxWidth(width);
    node->measure(renderer);
    place(node, x, y);
  };
  label(
      m_heading,
      i18n::tr(activity ? "desktop-widgets.editor.types.screen-time" : "desktop-widgets.editor.types.batteries"), 0, 0,
      card.width, Style::fontSizeCaption
  );
  const float heroWidth = small || large ? card.width : card.width * 0.43F;
  label(m_valueLabel, m_value, 0, 34 * scale, heroWidth, small ? 42 : 48);
  label(m_detailLabel, m_detail, 0, m_value.empty() ? 85 * scale : 96 * scale, heroWidth, Style::fontSizeCaption);
  m_symbol->setVisible(m_value.empty());
  m_symbol->setGlyphSize(32 * scale);
  m_symbol->layout(renderer);
  place(m_symbol, 0, 36 * scale);
  const std::size_t count = small ? 0 : std::min(m_rows.size(), m_items.size());
  const float rowsX = large ? 0 : card.width * 0.48F;
  const float rowsY = large ? (activity ? 205.0F : 145.0F) * scale : 30 * scale;
  const float rowHeight = 39 * scale;
  const float rowsWidth = card.width - rowsX;
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    auto& row = m_rows[i];
    const bool visible = i < count && rowsY + static_cast<float>(i + 1) * rowHeight <= card.height;
    row.title->setVisible(visible);
    row.detail->setVisible(visible);
    row.glyph->setVisible(visible);
    row.bar->setVisible(visible);
    if (!visible)
      continue;
    const auto& item = m_items[i];
    const float y = rowsY + static_cast<float>(i) * rowHeight;
    row.glyph->setGlyph(item.glyph);
    row.glyph->setGlyphSize(18 * scale);
    row.glyph->layout(renderer);
    place(row.glyph, rowsX, y);
    label(row.title, item.title, rowsX + 27 * scale, y, std::max(1.0F, rowsWidth - 90 * scale), Style::fontSizeCaption);
    row.detail->setTextAlign(TextAlign::End);
    label(row.detail, item.detail, card.width - 60 * scale, y, 60 * scale, Style::fontSizeCaption);
    row.bar->setProgress(item.progress);
    row.bar->setSize(rowsWidth - 27 * scale, 3 * scale);
    place(row.bar, rowsX + 27 * scale, y + 23 * scale);
  }
  const float chartY = large ? 142 * scale : card.height - 39 * scale;
  const float chartWidth = !small && !large ? heroWidth : card.width;
  for (std::size_t i = 0; i < m_bars.size(); ++i) {
    auto* bar = m_bars[i];
    bar->setVisible(activity && !m_value.empty());
    const float height = std::max(2.0F * scale, m_history[i] * 32 * scale);
    bar->setSize(std::max(1.0F, chartWidth / 24 - 2 * scale), height);
    place(bar, static_cast<float>(i) * chartWidth / 24, chartY + 32 * scale - height);
  }
  root()->setSize(card.width, card.height);
}

void DesktopStatusCardWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  for (auto* label : {m_heading, m_valueLabel, m_detailLabel})
    if (label)
      label->setFontFamily(family);
  for (auto& row : m_rows) {
    if (row.title)
      row.title->setFontFamily(family);
    if (row.detail)
      row.detail->setFontFamily(family);
  }
}
