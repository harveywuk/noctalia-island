# What still looks un-Mac-like on the merged shell

Branch `feature/orbit-island` at d9b6be4 (all of PRs #1 to #21), light and dark, headless Sway at 1920x1080, Inter (no SF Pro), no blur, Papirus icons.
Screenshots are in [assets/merged-survey](assets/merged-survey/). Ranked by how often you'd see each one.

## Clear-cut (fixed on this branch)

After screenshots: [monogram](assets/merged-survey/fixes/control-center-monogram.png), [Light first](assets/merged-survey/fixes/theme-mode-light-first.png), [chevrons](assets/merged-survey/fixes/settings-index-chevrons.png), [empty wallpaper panel](assets/merged-survey/fixes/wallpaper-empty-state.png).

1. **Control Center profile card has a hole where the avatar goes.** With no avatar picture set, the card shows an empty space and the name floats to the right of it ([control-center-light.png](assets/merged-survey/control-center-light.png), [control-center-dark.png](assets/merged-survey/control-center-dark.png)). macOS shows a grey circle with your initials, and the lock screen here already does. Fix: the same initials circle in the Control Center.
2. **Light and Dark are in the wrong order.** Theme Mode reads "Dark | Light | Auto" and Shell Theme Mode "Follow | Dark | Light | Auto" ([settings-theme-order.png](assets/merged-survey/settings-theme-order.png)); the wallpaper panel and setup wizard do the same. System Settings > Appearance reads Light, Dark, Auto. Fix: put Light first everywhere.
3. **Settings index rows put the chevron next to the word.** Appearance's index reads "Theme  ›", "Interface  ›" with the chevron right after the label ([settings-appearance-index.png](assets/merged-survey/settings-appearance-index.png)). In System Settings the chevron sits at the right edge of the row. Same on the Plugins page.
4. **The wallpaper panel opens to a blank box when the folder has no images** ([wallpaper-empty.png](assets/merged-survey/wallpaper-empty.png)). macOS always says what's missing. Fix: a "No Wallpapers" message naming the folder it looked in.

## Picked by harvey (also fixed on this branch)

After screenshots: [banner icon](assets/merged-survey/fixes/island-banner-large-icon.png), [sidebar without headings](assets/merged-survey/fixes/settings-sidebar-no-headings.png). The switcher now stays hidden when no windows are open.

5. **Island notification banner icon is tiny.** The banner shows a 16 px icon beside the app name ([island-banner-icons.png](assets/merged-survey/island-banner-icons.png)); a macOS banner leads with a large app icon (about 38 px) next to the title and body. Proposal: a larger leading icon, keeping the black capsule.
6. **Settings sidebar has group headings** ("Personalise", "Devices", "Windows & workspaces", "General"). Ventura's sidebar has no headings, only gaps between groups. Proposal: drop the headings and keep the gaps.
7. **Window switcher with no windows** shows a "0 windows" chip and "No open windows" over the screen ([window-switcher-empty.png](assets/merged-survey/window-switcher-empty.png)). Under Sway it stayed up until closed by command, because Sway gives it no window list. On Hyprland it should close when you let go of the modifier; please check. Proposal: don't open it at all when there are no windows, as Cmd+Tab never shows an empty strip.

## Needs your machine

- Notification Centre cards look grey in light mode without blur ([notification-centre-light.png](assets/merged-survey/notification-centre-light.png)). With Hyprland's blur they may look right; tell me if they look dull.
- Notifications that name their icon (for example Firefox's) showed a bell here, because this container's icon theme lookup didn't find them. Icons given by path or desktop entry work ([island-banner-icons.png](assets/merged-survey/island-banner-icons.png)). Check whether your notifications show app icons.
- The volume and brightness popups, the lock screen and real window switching can't run in this container.

## Already close

Launcher and its /clip, /snip and web fallback ([launcher-modes.png](assets/merged-survey/launcher-modes.png)), session cards, password prompt, Copied pill and script activity ([island-pills.png](assets/merged-survey/island-pills.png)), tray drawer empty state, Settings sidebar icons, dark mode across panels.

## Small and not about looks

- A pinned dock app written with ".desktop" on the end (for example `org.codeberg.dnkl.foot.desktop`) shows a blank placeholder; the docs say it should match. Easy to fix if you want it.

## Second pass (3 October 2026, evening): launcher and panels on the merged branch with the Raycast changes

Headless Sway at 1920x1080, Inter, Papirus, light and dark. Screenshots of the launcher are in [assets/launcher-raycast](assets/launcher-raycast/).

Fixed on this branch:

- The `/` overview put its internal provider id (`__launcher_provider_overview__`) in the action bar's kind label. Overview rows now read **Command** there and at the trailing edge.
- Going back to the root search from a short or empty provider view (Esc from `/clip`) scrolled the first section header out of view: the grid brought the selected row into a viewport that still had the shorter list's height. The grid now never scrolls a row's own top away.
- An empty provider view said "No results found" with nothing typed; it now says "Nothing in Clipboard History yet".

Still as before, and fine: the launcher card, section headers, kind labels and the action bar match Raycast's proportions in both themes; Control Center, the notification stack, session cards, the wallpaper picker, Settings and the dock read as Ventura. The sway title bar above Settings and the missing dock magnification are the headless compositor, not the shell.
