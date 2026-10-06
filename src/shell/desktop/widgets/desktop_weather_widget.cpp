#include "shell/desktop/widgets/desktop_weather_widget.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>

namespace {

  constexpr float kBaseWidth = 180.0F;
  constexpr float kBaseHeight = 72.0F;
  constexpr float kGlyphSlotWidth = 52.0F;
  constexpr float kColumnGap = 8.0F;
  constexpr float kLineGap = 2.0F;
  constexpr float kForecastSectionGap = 6.0F;
  constexpr float kForecastRowHeight = 20.0F;
  constexpr float kForecastDayWidth = 34.0F;
  constexpr float kForecastGlyphSlotWidth = 24.0F;

  float temperatureFontSize(float contentScale) { return Style::fontSizeBody * 2.25F * contentScale; }
  float conditionFontSize(float contentScale) { return Style::fontSizeBody * contentScale; }
  float glyphFontSize(float contentScale) { return Style::fontSizeBody * 3.0F * contentScale; }
  float forecastFontSize(float contentScale) { return Style::fontSizeCaption * contentScale; }
  float forecastGlyphFontSize(float contentScale) { return Style::fontSizeBody * 1.25F * contentScale; }

  std::string todayIso(std::int32_t utcOffsetSeconds) {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    return formatStrftime("%Y-%m-%d", tm);
  }

  std::string weekdayAbbrev(const std::string& isoDate) {
    if (isoDate.size() != 10) {
      return isoDate;
    }

    std::tm tm{};
    tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(isoDate.substr(8, 2));
    if (std::mktime(&tm) == -1) {
      return isoDate;
    }

    const std::string weekday = formatStrftime("%a", tm);
    return weekday.empty() ? isoDate : weekday;
  }

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopWeatherWidget::DesktopWeatherWidget(const WeatherService* weather, Options options)
    : m_weather(weather), m_color(options.color), m_shadow(options.shadow), m_showForecast(options.showForecast),
      m_forecastDays(std::clamp(options.forecastDays, 1, static_cast<int>(kMaxForecastRows))),
      m_cardSize(options.cardSize) {}

void DesktopWeatherWidget::create() {
  auto rootNode = ui::inputArea({});
  rootNode->setHitTestVisible(canShowDetails());
  if (canShowDetails()) {
    rootNode->setOnClick([this](const InputArea::PointerData&) {
      requestDetails({.kind = DesktopWidgetDetailsRequest::Kind::Weather});
    });
    rootNode->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    rootNode->setTooltip(i18n::tr("desktop-widgets.details.weather"));
  }
  rootNode->setClipChildren(true);

  rootNode->addChild(
      ui::label({
          .out = &m_location,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .visible = usesCardLayout(),
      })
  );
  rootNode->addChild(
      ui::label({
          .out = &m_range,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = usesCardLayout(),
      })
  );
  rootNode->addChild(
      ui::label({
          .out = &m_forecastTitle,
          .text = i18n::tr("desktop-widgets.weather.forecast"),
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = false,
      })
  );
  rootNode->addChild(
      ui::box({
          .out = &m_separator,
          .fill = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .visible = false,
      })
  );

  auto glyph = ui::glyph({
      .out = &m_glyph,
      .glyph = "weather-cloud",
      .glyphSize = glyphFontSize(contentScale()),
      .color = m_color,
  });
  rootNode->addChild(std::move(glyph));

  auto temperature = ui::label({
      .out = &m_temperature,
      .fontSize = temperatureFontSize(contentScale()),
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(temperature));

  auto condition = ui::label({
      .out = &m_condition,
      .fontSize = conditionFontSize(contentScale()),
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(condition));

  for (auto& row : m_forecastRows) {
    auto day = ui::label({
        .out = &row.day,
        .fontSize = forecastFontSize(contentScale()),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    rootNode->addChild(std::move(day));

    auto forecastGlyph = ui::glyph({
        .out = &row.glyph,
        .glyph = "weather-cloud",
        .glyphSize = forecastGlyphFontSize(contentScale()),
        .color = m_color,
        .visible = false,
    });
    rootNode->addChild(std::move(forecastGlyph));

    auto temps = ui::label({
        .out = &row.temps,
        .fontSize = forecastFontSize(contentScale()),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    rootNode->addChild(std::move(temps));
    rootNode->addChild(
        ui::label(
            {.out = &row.low, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1, .visible = false}
        )
    );
    rootNode->addChild(ui::label({.out = &row.high, .color = m_color, .maxLines = 1, .visible = false}));
    rootNode->addChild(
        ui::box(
            {.out = &row.rangeTrack, .fill = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12F), .visible = false}
        )
    );
    rootNode->addChild(
        ui::box({.out = &row.rangeFill, .fill = colorSpecFromRole(ColorRole::Primary, 0.75F), .visible = false})
    );
  }

  setRoot(std::move(rootNode));
  applyShadow();
}

bool DesktopWeatherWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "card_size") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_cardSize = desktop_cards::sizeFromSetting(*v);
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_glyph != nullptr) {
        m_glyph->setColor(m_color);
      }
      if (m_temperature != nullptr) {
        m_temperature->setColor(m_color);
      }
      if (m_condition != nullptr) {
        m_condition->setColor(m_color);
      }
      for (auto& row : m_forecastRows) {
        if (row.glyph != nullptr) {
          row.glyph->setColor(m_color);
        }
        if (row.day != nullptr) {
          row.day->setColor(m_color);
        }
        if (row.temps != nullptr) {
          row.temps->setColor(m_color);
        }
        if (row.high != nullptr) {
          row.high->setColor(m_color);
        }
      }
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      return true;
    }
    return false;
  }
  if (key == "show_forecast") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showForecast = *v;
      sync();
      syncForecast(renderer);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  if (key == "forecast_days") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_forecastDays = std::clamp(static_cast<int>(*v), 1, static_cast<int>(kMaxForecastRows));
      syncForecast(renderer);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopWeatherWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  for (Label* label : {m_location, m_range, m_forecastTitle}) {
    if (label != nullptr)
      label->setFontFamily(family);
  }
  if (m_temperature != nullptr) {
    m_temperature->setFontFamily(family);
  }
  if (m_condition != nullptr) {
    m_condition->setFontFamily(family);
  }
  for (const auto& row : m_forecastRows) {
    if (row.low != nullptr)
      row.low->setFontFamily(family);
    if (row.high != nullptr)
      row.high->setFontFamily(family);
    if (row.day != nullptr) {
      row.day->setFontFamily(family);
    }
    if (row.temps != nullptr) {
      row.temps->setFontFamily(family);
    }
  }
}

void DesktopWeatherWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }

  m_location->setVisible(usesCardLayout());
  m_range->setVisible(usesCardLayout());
  m_forecastTitle->setVisible(false);
  m_separator->setVisible(false);
  if (usesCardLayout()) {
    layoutCard(renderer);
    return;
  }
  m_temperature->setFontWeight(FontWeight::Bold);
  m_glyph->setColor(m_color);
  for (auto& row : m_forecastRows)
    row.glyph->setColor(m_color);

  const float scale = contentScale();
  const float width = kBaseWidth * scale;
  const float currentHeight = kBaseHeight * scale;
  const int forecastRowCount = m_showForecast ? std::clamp(m_forecastDays, 1, static_cast<int>(kMaxForecastRows)) : 0;
  const float forecastHeight = forecastRowCount > 0
      ? (kForecastSectionGap + static_cast<float>(forecastRowCount) * kForecastRowHeight) * scale
      : 0.0F;
  const float height = currentHeight + forecastHeight;

  const float glyphSlotWidth = kGlyphSlotWidth * scale;
  const float textX = glyphSlotWidth + kColumnGap * scale;
  const float textWidth = std::max(1.0F, width - textX);

  m_temperature->setFontSize(temperatureFontSize(scale));
  m_temperature->setMaxWidth(textWidth);
  m_temperature->setMaxLines(1);

  m_condition->setFontSize(conditionFontSize(scale));
  m_condition->setMaxWidth(textWidth);
  m_condition->setMaxLines(1);

  m_glyph->setGlyphSize(glyphFontSize(scale));
  applyShadow();

  sync();
  syncForecast(renderer);

  m_temperature->measure(renderer);
  m_condition->measure(renderer);
  m_glyph->measure(renderer);

  const bool hasCondition = !m_condition->text().empty();
  const float lineGap = hasCondition ? kLineGap * scale : 0.0F;
  float textHeight = m_temperature->height();
  if (hasCondition) {
    textHeight += lineGap + m_condition->height();
  }

  m_glyph->setPosition(
      std::round((glyphSlotWidth - m_glyph->width()) * 0.5F), std::round((currentHeight - m_glyph->height()) * 0.5F)
  );

  float y = std::round((currentHeight - textHeight) * 0.5F);
  m_temperature->setPosition(textX, y);
  y += std::round(m_temperature->height() + lineGap);
  m_condition->setPosition(textX, y);

  if (forecastRowCount > 0) {
    const float dayWidth = kForecastDayWidth * scale;
    const float glyphWidth = kForecastGlyphSlotWidth * scale;
    const float rowHeight = kForecastRowHeight * scale;
    const float forecastStartY = currentHeight + kForecastSectionGap * scale;
    const float forecastFont = forecastFontSize(scale);
    const float forecastGlyphSize = forecastGlyphFontSize(scale);
    const float tempsX = dayWidth + glyphWidth + kColumnGap * scale;
    const float tempsWidth = std::max(1.0F, width - tempsX);

    for (int i = 0; i < forecastRowCount; ++i) {
      auto& row = m_forecastRows[static_cast<std::size_t>(i)];
      if (row.day == nullptr || row.glyph == nullptr || row.temps == nullptr) {
        continue;
      }

      row.day->setFontSize(forecastFont);
      row.day->setMaxWidth(dayWidth);
      row.temps->setFontSize(forecastFont);
      row.temps->setMaxWidth(tempsWidth);
      row.glyph->setGlyphSize(forecastGlyphSize);

      row.day->measure(renderer);
      row.temps->measure(renderer);
      row.glyph->measure(renderer);

      const float rowY = forecastStartY + static_cast<float>(i) * rowHeight;
      row.day->setPosition(0.0F, std::round(rowY + (rowHeight - row.day->height()) * 0.5F));
      row.glyph->setPosition(
          dayWidth + std::round((glyphWidth - row.glyph->width()) * 0.5F),
          std::round(rowY + (rowHeight - row.glyph->height()) * 0.5F)
      );
      row.temps->setPosition(tempsX, std::round(rowY + (rowHeight - row.temps->height()) * 0.5F));
    }
  }

  root()->setSize(width, height);
}

desktop_cards::Layout DesktopWeatherWidget::cardLayout() const noexcept {
  return desktop_cards::resolve(m_cardSize, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
}

int DesktopWeatherWidget::forecastRowCount() const noexcept {
  if (!usesCardLayout())
    return m_showForecast ? m_forecastDays : 0;
  const auto size = cardLayout().size;
  return size == desktop_cards::Size::Small ? 0 : (size == desktop_cards::Size::Medium ? 3 : 6);
}

void DesktopWeatherWidget::layoutCard(Renderer& renderer) {
  const auto card = cardLayout();
  const float scale = card.scale;
  const bool medium = card.size == desktop_cards::Size::Medium;
  const bool large = card.size == desktop_cards::Size::Large;
  const float summaryWidth = medium ? 156.0F * scale : card.width;
  const float summaryHeight = large ? 152.0F * scale : card.height;
  const auto place = [&](Label* label, float x, float y, float width, float fontSize) {
    label->setFontSize(fontSize * scale);
    label->setMaxWidth(std::max(1.0F, width));
    label->measure(renderer);
    label->setPosition(Style::rtl() ? card.width - x - label->width() : x, y);
  };
  sync();
  syncForecast(renderer);
  applyShadow();
  m_temperature->setFontWeight(FontWeight::Normal);
  m_glyph->setColor(colorSpecFromRole(ColorRole::Primary));
  const bool hasData =
      m_weather != nullptr && m_weather->enabled() && m_weather->locationConfigured() && m_weather->hasData();
  place(m_location, 0.0F, 0.0F, summaryWidth, Style::fontSizeTitle);
  place(m_temperature, 0.0F, 28.0F * scale, summaryWidth, hasData ? Style::fontSizeBody * 4.0F : Style::fontSizeHeader);
  const float conditionY = summaryHeight - 46.0F * scale;
  place(m_condition, 0.0F, conditionY, summaryWidth, Style::fontSizeBody);
  place(m_range, 0.0F, summaryHeight - 22.0F * scale, summaryWidth, Style::fontSizeCaption);
  m_glyph->setGlyphSize((large ? 64.0F : 34.0F) * scale);
  m_glyph->measure(renderer);
  const float glyphX = summaryWidth - m_glyph->width();
  m_glyph->setPosition(
      Style::rtl() ? card.width - glyphX - m_glyph->width() : glyphX, large ? 36.0F * scale : 72.0F * scale
  );

  const bool forecast = (medium || large) && hasData;
  m_separator->setVisible(forecast);
  m_forecastTitle->setVisible(forecast);
  const float forecastX = medium ? summaryWidth + 2.0F * Style::spaceMd * scale : 0.0F;
  const float forecastY = large ? summaryHeight + Style::spaceMd * scale : 0.0F;
  const float forecastWidth = card.width - forecastX;
  if (forecast) {
    place(m_forecastTitle, forecastX, forecastY, forecastWidth, Style::fontSizeCaption);
    const float separatorX = medium ? summaryWidth + Style::spaceMd * scale : 0.0F;
    m_separator->setSize(
        medium ? Style::borderWidth * scale : card.width, medium ? card.height : Style::borderWidth * scale
    );
    m_separator->setPosition(
        Style::rtl() ? card.width - separatorX - m_separator->width() : separatorX, medium ? 0.0F : summaryHeight
    );
  }
  const int count = forecastRowCount();
  double minimum = 0.0;
  double maximum = 0.0;
  bool first = true;
  for (const auto& row : m_forecastRows) {
    if (!row.day->visible())
      continue;
    minimum = first ? row.minimum : std::min(minimum, row.minimum);
    maximum = first ? row.maximum : std::max(maximum, row.maximum);
    first = false;
  }
  const double span = std::max(1.0, maximum - minimum);
  const float rowHeight = large ? 32.0F * scale : 42.0F * scale;
  const float rowsY = forecastY + 28.0F * scale;
  for (int i = 0; i < count; ++i) {
    auto& row = m_forecastRows[static_cast<std::size_t>(i)];
    const float y = rowsY + static_cast<float>(i) * rowHeight;
    place(row.day, forecastX, y + 7.0F * scale, 48.0F * scale, Style::fontSizeBody);
    row.glyph->setGlyphSize(20.0F * scale);
    row.glyph->setColor(colorSpecFromRole(ColorRole::Primary));
    row.glyph->measure(renderer);
    const float x = forecastX + 52.0F * scale;
    row.glyph->setPosition(Style::rtl() ? card.width - x - row.glyph->width() : x, y + 4.0F * scale);
    if (large) {
      place(row.low, 92.0F * scale, y + 7.0F * scale, 40.0F * scale, Style::fontSizeBody);
      place(row.high, card.width - 34.0F * scale, y + 7.0F * scale, 34.0F * scale, Style::fontSizeBody);
      const float trackX = 140.0F * scale;
      const float trackWidth = std::max(1.0F, card.width - trackX - 48.0F * scale);
      const float start = static_cast<float>((row.minimum - minimum) / span) * trackWidth;
      const float length = std::max(4.0F * scale, static_cast<float>((row.maximum - row.minimum) / span) * trackWidth);
      row.rangeTrack->setSize(trackWidth, 4.0F * scale);
      row.rangeTrack->setRadius(2.0F * scale);
      row.rangeTrack->setPosition(Style::rtl() ? card.width - trackX - trackWidth : trackX, y + 12.0F * scale);
      row.rangeFill->setSize(std::min(length, trackWidth - start), 4.0F * scale);
      row.rangeFill->setRadius(2.0F * scale);
      const float fillX = trackX + start;
      row.rangeFill->setPosition(Style::rtl() ? card.width - fillX - row.rangeFill->width() : fillX, y + 12.0F * scale);
    }
    const float tempsX = forecastX + 82.0F * scale;
    place(row.temps, tempsX, y + 7.0F * scale, std::max(1.0F, forecastWidth - 82.0F * scale), Style::fontSizeBody);
  }
  root()->setSize(card.width, card.height);
}

void DesktopWeatherWidget::doUpdate(Renderer& renderer) {
  bool changed = sync();
  if (forecastRowCount() > 0) {
    changed = syncForecast(renderer) || changed;
  }
  if (changed) {
    requestLayout();
  }
}

void DesktopWeatherWidget::applyShadow() {
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
  const auto applyToLabel = [this, shadow](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow && !usesCardLayout()) {
      const float offset = kShadowOffset * contentScale();
      label->setShadow(shadow, offset, offset);
    } else {
      label->clearShadow();
    }
  };

  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }
  if (m_shadow && !usesCardLayout()) {
    const float offset = kShadowOffset * contentScale();
    m_glyph->setShadow(shadow, offset, offset);
    m_temperature->setShadow(shadow, offset, offset);
    m_condition->setShadow(shadow, offset, offset);
  } else {
    m_glyph->clearShadow();
    m_temperature->clearShadow();
    m_condition->clearShadow();
  }

  for (const auto& row : m_forecastRows) {
    applyToLabel(row.day);
    applyToLabel(row.temps);
    if (row.glyph != nullptr) {
      if (m_shadow && !usesCardLayout()) {
        const float offset = kShadowOffset * contentScale();
        row.glyph->setShadow(shadow, offset, offset);
      } else {
        row.glyph->clearShadow();
      }
    }
  }
}

bool DesktopWeatherWidget::sync() {
  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return false;
  }

  std::string glyphName = "weather-cloud";
  std::string temperatureText = "--";
  std::string conditionText;
  std::string locationText = i18n::tr("desktop-widgets.editor.types.weather");
  std::string rangeText;

  if (m_weather == nullptr || !m_weather->enabled()) {
    temperatureText = i18n::tr("desktop-widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    temperatureText = i18n::tr("desktop-widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    locationText = snapshot.locationName.empty() ? locationText : snapshot.locationName;
    glyphName = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    temperatureText = std::format("{}{}", temp, m_weather->displayTemperatureUnit());
    conditionText = WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
    if (!snapshot.forecastDays.empty()
        && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds)) {
      const auto& day = snapshot.forecastDays.front();
      rangeText = i18n::tr(
          "desktop-widgets.weather.high-low", "high",
          std::format(
              "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
              m_weather->displayTemperatureUnit()
          ),
          "low",
          std::format(
              "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
              m_weather->displayTemperatureUnit()
          )
      );
    }
  } else if (m_weather->loading()) {
    temperatureText = i18n::tr("desktop-widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    temperatureText = i18n::tr("desktop-widgets.weather.error");
  }

  bool changed = false;
  changed = m_location->setText(locationText) || changed;
  changed = m_range->setText(rangeText) || changed;

  if (glyphName != m_lastGlyph) {
    m_lastGlyph = glyphName;
    m_glyph->setGlyph(glyphName);
    changed = true;
  }

  if (temperatureText != m_lastTemperature) {
    m_lastTemperature = temperatureText;
    m_temperature->setText(temperatureText);
    changed = true;
  }

  if (conditionText != m_lastCondition) {
    m_lastCondition = conditionText;
    m_condition->setText(conditionText);
    changed = true;
  }

  return changed;
}

bool DesktopWeatherWidget::syncForecast(Renderer& renderer) {
  const int rowCount = forecastRowCount();
  const bool hasWeatherData =
      m_weather != nullptr && m_weather->enabled() && m_weather->locationConfigured() && m_weather->hasData();
  std::size_t forecastStart = 0;
  if (hasWeatherData) {
    const auto& snapshot = m_weather->snapshot();
    forecastStart =
        !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds)
        ? 1
        : 0;
  }
  bool changed = false;

  for (std::size_t i = 0; i < kMaxForecastRows; ++i) {
    auto& row = m_forecastRows[i];
    if (row.day == nullptr || row.glyph == nullptr || row.temps == nullptr) {
      continue;
    }

    const std::size_t dayIndex = forecastStart + i;
    const bool visible =
        hasWeatherData && static_cast<int>(i) < rowCount && dayIndex < m_weather->snapshot().forecastDays.size();
    if (row.day->visible() != visible) {
      changed = true;
    }
    row.day->setVisible(visible);
    row.glyph->setVisible(visible);
    const bool ranges = usesCardLayout() && cardLayout().size == desktop_cards::Size::Large;
    row.temps->setVisible(visible && !ranges);
    row.low->setVisible(visible && ranges);
    row.high->setVisible(visible && ranges);
    row.rangeTrack->setVisible(visible && ranges);
    row.rangeFill->setVisible(visible && ranges);
    if (!visible) {
      continue;
    }

    const auto& snapshot = m_weather->snapshot();
    const auto& day = snapshot.forecastDays[dayIndex];
    row.minimum = m_weather->displayTemperature(day.temperatureMinC);
    row.maximum = m_weather->displayTemperature(day.temperatureMaxC);
    changed = row.low->setText(std::format("{}°", static_cast<int>(std::lround(row.minimum)))) || changed;
    changed = row.high->setText(std::format("{}°", static_cast<int>(std::lround(row.maximum)))) || changed;
    const std::string dayText = weekdayAbbrev(day.dateIso);
    const std::string glyphName = WeatherService::glyphForCode(day.weatherCode, true);
    const std::string tempsText = std::format(
        "{} / {}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
        static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
        m_weather->displayTemperatureUnit()
    );

    if (dayText != row.lastDay) {
      row.lastDay = dayText;
      row.day->setText(dayText);
      row.day->measure(renderer);
      changed = true;
    }
    if (glyphName != row.lastGlyph) {
      row.lastGlyph = glyphName;
      row.glyph->setGlyph(glyphName);
      row.glyph->measure(renderer);
      changed = true;
    }
    if (tempsText != row.lastTemps) {
      row.lastTemps = tempsText;
      row.temps->setText(tempsText);
      row.temps->measure(renderer);
      changed = true;
    }
  }

  return changed;
}
