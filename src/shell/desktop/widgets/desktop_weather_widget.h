#pragma once

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <array>
#include <cstddef>
#include <string>

class Glyph;
class Box;
class Label;
class Renderer;
class WeatherService;

class DesktopWeatherWidget : public DesktopWidget {
public:
  struct Options {
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool showForecast = false;
    int forecastDays = 3;
    desktop_cards::Size cardSize = desktop_cards::Size::Classic;
  };

  DesktopWeatherWidget(const WeatherService* weather, Options options);

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  struct ForecastRow {
    Glyph* glyph = nullptr;
    Label* day = nullptr;
    Label* temps = nullptr;
    Label* low = nullptr;
    Label* high = nullptr;
    Box* rangeTrack = nullptr;
    Box* rangeFill = nullptr;
    double minimum = 0.0;
    double maximum = 0.0;
    std::string lastDay;
    std::string lastGlyph;
    std::string lastTemps;
  };

  void doLayout(Renderer& renderer) override;
  void layoutCard(Renderer& renderer);
  [[nodiscard]] bool usesCardLayout() const noexcept override { return m_cardSize != desktop_cards::Size::Classic; }
  [[nodiscard]] desktop_cards::Layout cardLayout() const noexcept;
  [[nodiscard]] int forecastRowCount() const noexcept;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool sync();
  bool syncForecast(Renderer& renderer);
  void applyShadow();

  static constexpr std::size_t kMaxForecastRows = 6;

  const WeatherService* m_weather = nullptr;
  ColorSpec m_color;
  bool m_shadow;
  bool m_showForecast;
  int m_forecastDays;
  desktop_cards::Size m_cardSize;

  Glyph* m_glyph = nullptr;
  Label* m_temperature = nullptr;
  Label* m_condition = nullptr;
  Label* m_location = nullptr;
  Label* m_range = nullptr;
  Label* m_forecastTitle = nullptr;
  Box* m_separator = nullptr;
  std::array<ForecastRow, kMaxForecastRows> m_forecastRows{};

  std::string m_lastGlyph;
  std::string m_lastTemperature;
  std::string m_lastCondition;
};
