#pragma once

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/controls/calendar_view.h"

#include <cstdint>
#include <limits>
#include <memory>

class Button;
class CalendarService;
class ConfigService;
class Flex;
class InputArea;
class Label;
class Renderer;
class ScrollView;

class DesktopCalendarWidget : public DesktopWidget {
public:
  struct Options {
    bool showEvents = true;
    bool showWeekNumbers = false;
    desktop_cards::Size cardSize = desktop_cards::Size::Classic;
  };

  DesktopCalendarWidget(ConfigService* config, CalendarService* calendar, Options options);
  ~DesktopCalendarWidget() override;

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void layoutCard(Renderer& renderer);
  [[nodiscard]] bool usesCardLayout() const noexcept override { return m_cardSize != desktop_cards::Size::Classic; }
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void changeMonthBy(int delta);
  void focusToday();
  void rebuildCalendar();
  void rebuildEventList();
  void markDirty();

  ConfigService* m_config = nullptr;
  CalendarService* m_calendar = nullptr;
  bool m_showEvents = true;
  bool m_showWeekNumbers = false;
  bool m_dirty = true;
  desktop_cards::Size m_cardSize;
  float m_cardGridWidth = 0.0F;
  float m_cardDayHeight = 0.0F;
  float m_cardScale = 1.0F;

  Flex* m_rootLayout = nullptr;
  Flex* m_dateSummary = nullptr;
  Label* m_weekdayLabel = nullptr;
  Label* m_dayLabel = nullptr;
  Label* m_dateMonthLabel = nullptr;
  InputArea* m_calendarArea = nullptr;
  Flex* m_calendarColumn = nullptr;
  Flex* m_header = nullptr;
  Label* m_todayLabel = nullptr;
  Label* m_monthLabel = nullptr;
  Button* m_previousButton = nullptr;
  Button* m_nextButton = nullptr;
  Flex* m_grid = nullptr;
  Flex* m_eventsColumn = nullptr;
  Label* m_eventsTitle = nullptr;
  ScrollView* m_eventsScroll = nullptr;
  calendar_view::EventListState m_eventListState;

  int m_selectedYear = std::numeric_limits<int>::min();
  int m_selectedMonth = -1;
  int m_selectedDay = -1;
  int m_monthOffset = 0;
  int m_lastTodayKey = -1;
  std::uint64_t m_calendarCallbackId = 0;
};
