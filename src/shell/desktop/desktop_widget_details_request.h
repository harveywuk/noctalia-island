#pragma once

#include "ui/controls/calendar_view.h"

struct DesktopWidgetDetailsRequest {
  enum class Kind { Weather, Calendar, Batteries };
  Kind kind = Kind::Weather;
  calendar_view::Date date;
};
