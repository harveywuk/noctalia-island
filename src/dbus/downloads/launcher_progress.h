#pragma once

// LauncherEntry updates are partial. Disappearance alone can also mean cancel,
// pause or disconnect; only a reported 100% after observed work confirms success.
struct LauncherProgress {
  double progress = 0;
  bool visible = false;

  bool active() const { return visible && progress < 1.0; }
  bool completedBy(const LauncherProgress& next) const { return active() && next.progress >= 1.0; }
};
