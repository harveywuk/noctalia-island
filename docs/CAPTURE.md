# Native capture additions

The capture controls open inside the existing dynamic island surface when the
island is enabled. Drawing tools live in a compact menu, and the colour button
opens an expanding colour-and-size panel with swatches, HSV, hex/RGB entry, and
size presets. Escape dismisses a menu first, then closes capture. With the island
disabled, the same compact controls appear on the annotation overlay.

Built on the existing screenshot editor in the Orbit Island fork. UI, selection,
recording lifecycle, and island status are C++; video encoding uses wf-recorder.

- `noctalia msg screenshot-annotate`: freeze, draw, crop, copy, or save.
- The toolbar has a video-camera button for region recording and a monitor button
  for whole-monitor recording. Starting either leaves the current screenshot edit;
  save or copy it first if needed. While recording, either button stops it.
- Pixelate (`i`): drag a rectangle; size controls block width.
- Magnify (`z`): drag a rectangle; size controls zoom (default 2×, up to 8×).
- `noctalia msg record-region`: drag an area within one monitor.
- `noctalia msg record-monitor`: click the monitor's name in the picker.
- `noctalia msg record-stop`: finish and save the current recording.
- `noctalia msg record-status`: `idle`, recording timer, or saving state.
- Click the red recording timer in the island to stop. Escape cancels selection.

Recording defaults: desktop output audio only (explicit PulseAudio sink monitor),
60 fps, H.264/AAC MP4 under the XDG Videos directory's `Recordings` folder. NVIDIA
systems use NVENC; other systems use libx264. The selected audio output is fixed
for the recording; changing playback devices requires starting a new recording.
Region selection must stay within one output. This is SDR recording, not HDR
capture. Odd dimensions are padded to even sizes for codec compatibility.

Dependencies: wf-recorder with PulseAudio and FFmpeg support, pactl, xdg-user-dir.
Failure logs are retained beside the recording; successful recordings remove
the temporary log. Stop signals only the recorder child owned by this process.
No microphone capture or upload is configured.

Validation:

```sh
ninja -C build-rishot noctalia annotation_document_test annotation_raster_test screen_recorder_test island_state_test cli_schema_test cli_parse_test cli_help_test config_schema_roundtrip_test
meson test -C build-rishot --no-rebuild annotation_document annotation_raster screen_recorder island_state cli_schema cli_parse cli_help config_schema_roundtrip
python3 tests/capture_smoke.py
```

The smoke test uses a private headless Umbriel display and D-Bus session. It needs
GPU and desktop audio access. Artifacts are in `build-rishot/capture-smoke`.
