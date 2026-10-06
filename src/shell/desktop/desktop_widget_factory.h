#pragma once

#include "shell/desktop/desktop_battery_model.h"
#include "shell/desktop/desktop_widget.h"
#include "shell/desktop/desktop_widget_services.h"

#include <memory>
#include <string>
#include <unordered_map>

class HttpClient;
class CalendarService;
class MprisService;
class SystemMonitorService;
class PipeWireService;
class PipeWireSpectrum;
class WeatherService;

class DesktopWidgetFactory {
public:
  explicit DesktopWidgetFactory(DesktopWidgetRuntimeServices services);

  [[nodiscard]] std::unique_ptr<DesktopWidget> create(
      const std::string& type, const std::unordered_map<std::string, WidgetSettingValue>& settings,
      float contentScale = 1.0F, const std::vector<DesktopWidgetState>* states = nullptr, const std::string& id = {}
  ) const;

  [[nodiscard]] std::vector<desktop_batteries::Device> batteryDevices() const {
    return desktop_batteries::collect(m_services);
  }

private:
  CalendarService* m_calendar = nullptr;
  PipeWireService* m_pipewire = nullptr;
  PipeWireSpectrum* m_pipewireSpectrum = nullptr;
  const WeatherService* m_weather = nullptr;
  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  SystemMonitorService* m_sysmon = nullptr;
  DesktopWidgetScriptDeps m_scriptDeps;
  DesktopWidgetRuntimeServices m_services;
};
