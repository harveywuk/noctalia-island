#pragma once

#include "core/timer_manager.h"
#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

class Image;
class Label;
class Button;

class DesktopPhotosWidget final : public DesktopWidget {
public:
  DesktopPhotosWidget(std::string file, std::string folder, int intervalMinutes, desktop_cards::Size size);
  void create() override;

private:
  bool usesCardLayout() const noexcept override { return true; }
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void step(int direction);
  void sync(Renderer& renderer);
  std::string m_file;
  std::string m_folder;
  int m_intervalMinutes;
  desktop_cards::Size m_size;
  std::vector<std::string> m_files;
  std::size_t m_index = 0;
  std::string m_loaded;
  std::filesystem::file_time_type m_modified{};
  std::chrono::steady_clock::time_point m_lastScan{};
  Timer m_timer;
  Image* m_image = nullptr;
  Label* m_caption = nullptr;
  Label* m_empty = nullptr;
  Button* m_previous = nullptr;
  Button* m_next = nullptr;
  Button* m_open = nullptr;
};
