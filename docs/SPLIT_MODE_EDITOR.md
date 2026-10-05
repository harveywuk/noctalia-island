# Split modes in the Hyprland appearance editor

Plan for priority 3 in [DEVELOPMENT_HANDOVER.md](DEVELOPMENT_HANDOVER.md#proposed-next-priorities).
Written 5 October 2026 against `feature/orbit-island`. Nothing here is implemented yet.

## The problem

Since `57d56232d`, when `[theme].shell_mode` is pinned apart from `[theme].mode` (the desktop's
setting: a dark shell over light apps), the runtime splits the appearance profile. Window decoration
(blur, shadow, glow, dimming, border colours) comes from the profile for the apps' mode, and glass
comes from the profile for the shell's mode. `hyprlandAppearanceFor()` in
`src/app/application_services.cpp` does this by resolving both profiles and copying the eleven
`glass*` fields across.

The settings editor never learned about the split. Every place it resolves a look calls
`resolveAppearanceProfile(shell, isResolvedLightTheme())`, and `isResolvedLightTheme()` is the
**shell's** mode (`ThemeService` sets it from `m_isShellLightMode`). So with a dark shell over light
apps, the editor shows and acts on the dark profile while the windows wear the light one.

From reading the code (confirm each with the smoke test before fixing it):

| Where | What goes wrong under a dark shell and light apps |
|-------|---------------------------------------------------|
| `makeThemeProfilesEditor()`, `hyprland_editor.cpp:330` | The status line reads "Dark mode profile: Soft Glass Dark (built-in)", which is not what the windows show. |
| Same function, **Keep current look & edit** | Commits the dark profile as the manual look, so the windows jump from the light decoration to the dark one. The button promises the opposite. This is the most visible bug. |
| Presets, overview preset and curve editor, `hyprland_editor.cpp:1556` | Preset buttons start from the dark profile, and **Save profile** stores the dark look. |
| Undo snapshots, `settings_window_mutations.cpp:155, 186, 208, 234` | `m_hyprlandUndo` records the dark look, so **Undo** after a change can restore the wrong decoration. |
| `SettingsWindow::onThemeChanged()`, `settings_window.cpp:1261` | Rebuilds only when the shell's mode flips. With the shell pinned, an apps-mode change (the automatic schedule) leaves the status stale. |

When the two modes agree, every row above already behaves correctly; the fix must keep it that way.

## Approach

1. **One resolver for the effective look.** Move the merge out of `application_services.cpp` into
   `compositors/hyprland/hyprland_appearance.{h,cpp}`:

   ```cpp
   // The look Hyprland is given: decoration from the apps' profile, glass from the shell's.
   [[nodiscard]] HyprlandAppearanceConfig resolveEffectiveAppearance(const ShellConfig& shell,
                                                                     bool appsLight, bool shellLight);
   ```

   `hyprlandAppearanceFor()` keeps its Island-glass-layer logic and calls this, so the runtime and
   the editor cannot drift apart. Copy the glass fields in one helper (`copyGlass(from, to)`) so a
   future `glass_*` key is added in one place.

2. **Give the UI the apps' mode.** Add `isResolvedAppsLight()` / `setResolvedAppsLight()` beside
   `isResolvedLightTheme()` in `src/ui/palette.{h,cpp}`, set from `m_isLightMode` at the same two
   places in `theme_service.cpp` (around lines 541 and 555). This follows the existing pattern, so
   the mutation helpers reach it without threading a new field through `SettingsContentContext`.

3. **Use the effective look everywhere the editor resolves one.** Replace the six call sites above
   with `resolveEffectiveAppearance(shell, isResolvedAppsLight(), isResolvedLightTheme())`.

4. **Say what each part follows.** When the modes differ, the profile status shows two lines,
   for example "Windows follow light mode: Soft Glass Light (built-in)" and "Glass follows the
   dark shell: Soft Glass Dark (built-in)". New keys go in `assets/translations/en.json` (the other 26 catalogues fall
   back to English until translated). Run `python3 tools/i18n-check.py`.

5. **Rebuild on either mode.** `onThemeChanged()` tracks the apps' mode next to
   `m_hyprlandProfileLightMode` and rebuilds when either changes.

## Decisions to confirm before coding

- **What Keep current look and Save profile store under split modes.** Proposed: the effective
  look, i.e. what is on screen (light decoration with dark glass). The alternative, saving only
  the apps-mode profile, would lose the glass the user is looking at.
- **The manual sliders.** The registry builds them from `current.shell.hyprlandAppearance`, the
  manual values, not a resolved profile. Check what they show while automatic switching is on
  before deciding whether they need the split at all.

## Evidence the change needs

Following the working style: unit tests, the smoke run, and screenshots in both modes.

- **Unit (`tests/hyprland_appearance_test.cpp`):** with equal modes the effective look equals
  `resolveAppearanceProfile()`; with split modes every `glass_*` key comes from the shell's profile
  and every other key from the apps' (compare `appearanceTable()` output key by key, so a missed
  field fails the test).
- **Smoke (`tests/hyprland_smoke.py`, the theme-profile block near line 600):** with
  `mode = "light"` and `shell_mode = "dark"`, open `appearance/hyprland-theme-profiles`, check both
  status lines, click **Keep current look & edit**, and assert that `decoration:blur:brightness`
  stays at 1.04 (light decoration) and `plugin:hyprglass:default_theme` stays `dark`. Today that
  assertion should fail, which is the reproduction to capture first.
- **Screenshots:** the theme-profiles page before and after, for dark shell over light apps and for
  light shell over dark apps, in the harness or on DP-2.

## Done when

- The editor's status, presets, undo and **Keep current look** all match what Hyprland shows, in
  both split directions and with the modes equal.
- 153+ unit tests pass, the default smoke run passes with the plugins loaded
  (`NOCTALIA_TEST_HYPR_PLUGINS=~/.local/share/hyprland-plugins/0.56.2`), and `i18n-check` is clean.
- `docs/user/compositor-settings/hyprland.mdx` (Theme-linked appearance profiles) mentions what
  the editor shows under split modes.
