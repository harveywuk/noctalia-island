#pragma once

#include "core/files/file_watcher.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"
#include "shell/desktop/desktop_widget_services.h"
#include "shell/desktop/widgets/desktop_remote_source.h"

#include <array>
#include <filesystem>

class Label;
class Button;
class Checkbox;

class DesktopCollectionWidget final : public DesktopWidget {
public:
  using Settings = std::unordered_map<std::string, WidgetSettingValue>;
  DesktopCollectionWidget(std::string kind, Settings settings, DesktopWidgetRuntimeServices services);
  ~DesktopCollectionWidget() override;
  void create() override;

private:
  using Item = DesktopCardItem;
  struct Row {
    Button* action = nullptr;
    Label* detail = nullptr;
    Checkbox* check = nullptr;
  };
  bool usesCardLayout() const noexcept override { return true; }
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool refresh();
  void activate(std::size_t row);
  void toggle(std::size_t row, bool checked);
  std::string setting(const char* key, std::string fallback = {}) const;
  std::string m_kind;
  Settings m_settings;
  DesktopWidgetRuntimeServices m_services;
  desktop_cards::Size m_size;
  std::filesystem::path m_file;
  FileWatcher::WatchId m_watch = 0;
  std::vector<Item> m_items;
  std::size_t m_page = 0;
  std::size_t m_pageSize = 3;
  Label* m_heading = nullptr;
  Label* m_subtitle = nullptr;
  Label* m_empty = nullptr;
  Label* m_pageLabel = nullptr;
  Button* m_previous = nullptr;
  Button* m_next = nullptr;
  Button* m_open = nullptr;
  std::array<Row, 8> m_rows{};
  bool m_saveError = false;
  std::unique_ptr<DesktopRemoteSource> m_remote;
  std::filesystem::file_time_type m_fileTime{};
  bool m_fileLoaded = false;
  std::chrono::steady_clock::time_point m_lastDirectoryRead{};
};
