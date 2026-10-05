# Split modes in the Hyprland appearance editor

Implemented on `feature/orbit-island`, 5 October 2026. This fixes the editor follow-up in
[DEVELOPMENT_HANDOVER.md](DEVELOPMENT_HANDOVER.md).

## Behaviour

When apps and the shell use different modes, the editor uses the same combined appearance as
Hyprland: window decoration comes from the apps' profile and glass comes from the shell's.
The Theme profiles page identifies both sources, for example:

- **Windows follow light mode: Soft Glass Light (built-in)**
- **Glass follows the dark shell: Soft Glass Dark (built-in)**

When the modes agree, the existing single profile status remains. Missing or invalid profiles
fall back independently, with the warning below the affected profile's status.

**Keep current look & edit** and **Save current look** capture the combined appearance. Presets
start from that appearance, and undo restores it. Keeping a look, choosing a preset or undoing
pauses automatic switching; saving a named look leaves switching enabled. Manual sliders remain
hidden while automatic switching is on. Keeping the current look exposes them with the captured
values, so no change to the registry's manual-value handling was needed.

## Implementation

- `resolveEffectiveAppearance()` in `src/compositors/hyprland/hyprland_appearance.{h,cpp}` is shared
  by runtime application, the editor and all four undo snapshot paths. Its `copyGlass()` helper
  contains the eleven glass fields. The runtime retains its separate Island glass-layer override.
- `isResolvedAppsLight()` in `src/ui/palette.{h,cpp}` exposes the apps' resolved mode alongside
  the shell's. `ThemeService` publishes both before callbacks, including its unchanged-palette
  early returns when the shell is pinned.
- `SettingsWindow::onThemeChanged()` tracks both modes, so an app-mode change updates open
  editors even if the shell stays in the same mode.
- Four English translation keys describe the split status; other catalogues use the English
  fallback until translated. The [Hyprland guide](user/compositor-settings/hyprland.mdx) describes
  how the editor and saved looks behave.

## Regression evidence

The original release reproduced the bug in the isolated Hyprland harness: with light apps and a
dark shell, **Keep current look & edit** changed `decoration:blur:brightness` from `1.04` to `0.95`.
The reproduction's before/after screenshots are in the ignored build directory
`build-rishot/hyprland-smoke-j2djyszq/`.

The regression checks cover:

- `hyprland_appearance`: compare the entire serialized appearance against the expected combination
  in all four mode pairs, with built-in and saved profiles, management disabled, switching disabled,
  and missing or malformed profiles. Every glass field has distinct saved-profile fixtures so a
  forgotten field copy fails the test.
- `theme_shell_mode`: callbacks observe the resolved apps and shell flags, including immediate and
  animated updates where the pinned shell palette does not change.
- Default `hyprland_smoke.py`: check both status lines, updates while the page stays open, Keep,
  manual persistence across mode changes, saving without pausing switching, preset application and
  undo in all four mode pairs. It captures before/after screenshots for each pair and checks real
  Hyprland brightness and Hyprglass theme/blur values.

Useful commands:

```sh
meson compile -C build-test -j 6
meson test -C build-test --no-rebuild -j 6 --print-errorlogs
meson compile -C build-release -j 4
NOCTALIA_TEST_HYPR_PLUGINS="$HOME/.local/share/hyprland-plugins/0.56.2" python3 tests/hyprland_smoke.py
python3 tools/i18n-check.py
```

The smoke harness defaults to `build-rishot/noctalia` (locally linked to the release binary).
`NOCTALIA_TEST_BINARY` can select another build without changing that symlink.

Validation on 5 October: both debug and release builds completed; all 153 Meson tests passed
outside the socket-restricted sandbox; the full default smoke run passed with the installed
0.56.2 plugins; translation-key, changed C++ formatting and Python syntax checks passed.
The passing release run's logs and screenshots are in
`build-rishot/hyprland-smoke-udfo3v7a/`, including
`theme-light-apps-dark-shell-{before,after}.png` and
`theme-dark-apps-light-shell-{before,after}.png`. Both split directions were visually inspected.
The release build is local; it has not been installed on the development desktop.
