#pragma once

#include "render/core/texture_handle.h"

class LayerSurface;
struct wl_output;

// A panel borrows the live island surface; Island retains ownership.
struct IslandPanelSurface {
  LayerSurface* surface = nullptr;
  wl_output* output = nullptr;
  float width = 0;
  float height = 0;
  float scale = 1;
  // The Island's artwork gradient, while media plays (id 0 otherwise). It lives on this surface, so a
  // hosted panel can draw it behind its card as the capsule morphs, instead of a plain capsule.
  TextureHandle flow{};
  bool cupertino = false;
  bool compactLayout = false;
  bool mediaGradient = false;
};
