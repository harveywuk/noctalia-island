#pragma once

class LayerSurface;
struct wl_output;

// A panel borrows the live island surface; Island retains ownership.
struct IslandPanelSurface {
  LayerSurface* surface = nullptr;
  wl_output* output = nullptr;
  float width = 0;
  float height = 0;
  float scale = 1;
};
