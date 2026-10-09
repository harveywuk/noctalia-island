#pragma once

#include "shell/control_center/tab.h"

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

class AudioVisualizer;
class Button;
class Glyph;
class GridView;
class HttpClient;
class Image;
class MprisService;
class PipeWireSpectrum;
class ScrollView;

namespace control_center {

  // A temporary view of the available sections inside the Island's existing body.
  class SectionOverview final : public Tab {
  public:
    struct Section {
      int id;
      std::string key;
      std::string title;
      std::string glyph;
    };

    SectionOverview(MprisService* mpris, HttpClient* http, PipeWireSpectrum* spectrum);
    ~SectionOverview() override;
    void setSections(std::vector<Section> sections, std::function<void(int)> activate, int mediaId);
    std::unique_ptr<Flex> create() override;
    void setCurrent(int id);
    void setShown(int id, bool shown);
    void setActive(bool active) override;
    void onFrameTick(float deltaMs) override;
    void onClose() override;
    void focusCurrent();
    bool handleKey(std::uint32_t sym, std::uint32_t modifiers);

  private:
    void doLayout(Renderer& renderer, float width, float height) override;
    void doUpdate(Renderer& renderer) override;
    void syncSpectrum();
    void focusButton(Button* button);

    MprisService* m_mpris;
    HttpClient* m_http;
    PipeWireSpectrum* m_spectrum;
    std::vector<Section> m_sections;
    std::vector<Button*> m_buttons;
    std::function<void(int)> m_activate;
    int m_current = 0;
    int m_mediaId = 0;
    Flex* m_root = nullptr;
    ScrollView* m_scroll = nullptr;
    GridView* m_grid = nullptr;
    Flex* m_media = nullptr;
    Flex* m_mediaRow = nullptr;
    Flex* m_mediaText = nullptr;
    Button* m_mediaAction = nullptr;
    Image* m_art = nullptr;
    Glyph* m_artFallback = nullptr;
    Glyph* m_paused = nullptr;
    Label* m_track = nullptr;
    Label* m_artist = nullptr;
    AudioVisualizer* m_wave = nullptr;
    std::uint64_t m_spectrumListener = 0;
    bool m_active = false;
    bool m_playing = false;
    std::string m_artPath;
    std::unordered_set<std::string> m_pendingArt;
    std::shared_ptr<void> m_alive = std::make_shared<int>(0);
  };

} // namespace control_center
