#pragma once

#include "config/widget_setting_value.h"
#include "core/timer_manager.h"
#include "net/http_client.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct DesktopCardItem {
  std::string title;
  std::string detail;
  std::string action;
  bool checked = false;
  bool enabled = true;
  bool operator==(const DesktopCardItem&) const = default;
};

namespace desktop_sources {
  [[nodiscard]] std::vector<DesktopCardItem> parseFeed(std::string_view xml, bool podcasts);
  [[nodiscard]] bool validWebUrl(std::string_view url);
  [[nodiscard]] std::vector<DesktopCardItem>
  parseHomeStates(std::string_view json, const std::vector<std::string>& entities, bool locations);
  [[nodiscard]] std::vector<DesktopCardItem> parseQuote(std::string_view json);
} // namespace desktop_sources

// Opt-in network sources. Nothing is fetched until an endpoint or credential is configured.
class DesktopRemoteSource {
public:
  using Settings = std::unordered_map<std::string, WidgetSettingValue>;
  DesktopRemoteSource(std::string kind, Settings settings, HttpClient* http, std::function<void()> changed);
  ~DesktopRemoteSource();
  void start();
  void activate(const DesktopCardItem& item);
  [[nodiscard]] const std::vector<DesktopCardItem>& items() const { return m_items; }
  [[nodiscard]] const std::string& status() const { return m_status; }

private:
  void fetch();
  void fetchQuote(std::size_t index);
  void request(HttpRequest request, std::function<void(HttpResponse)> callback);
  std::string setting(const char* key) const;
  std::vector<std::string> list(const char* key) const;
  std::string token() const;
  std::string m_kind;
  Settings m_settings;
  HttpClient* m_http;
  std::function<void()> m_changed;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  Timer m_timer;
  bool m_pending = false;
  std::vector<DesktopCardItem> m_items;
  std::vector<DesktopCardItem> m_quotes;
  std::string m_status;
};
