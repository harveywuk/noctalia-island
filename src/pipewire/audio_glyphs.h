#pragma once

// Canonical shell glyph for a sink (isInput == false) or source (isInput == true) given its
// volume and effective mute. Single source of the speaker mute->slashed-icon rule and the
// volume-level thresholds so Control Center, bar widgets and the OSD cannot map the same state to different
// icons.
[[nodiscard]] const char* audioVolumeGlyph(float volume, bool muted, bool isInput);
