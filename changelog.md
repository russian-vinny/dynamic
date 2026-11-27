# v2.2.7 GitHub release and bare `dynamic` mod ID - 2026-11-26

- **user direction:** "i signed into github. i want u to upload all of this to github and make the .geode in the releases." Repository `russian-vinny/dynamic` is public; source only is uploaded and the built `.geode` is attached to release `v2.2.7`.
- **mod ID:** `mod.json` id changed from `dynamic.vinny` to the bare `dynamic` at the user's explicit request. Geode's `ModMetadata::validateID` allows a lowercase ID with no dot, so the package/asset is `dynamic.geode`. Name, developer and version stay `Dynamic` / `vinny` / `2.2.7`.
- **update URLs:** the silent version check now points at `russian-vinny/dynamic` for raw `version.txt`, release tags and `releases/latest/download/dynamic.geode`. Gameplay, UI, credits, settings keys, saved data and cube behavior are unchanged.
- **build:** the release SHA-256 is recorded in the vault's `01 Project/Build and Install.md`. The package keeps deflate compression and all 30 animated logo frames; older `dynamic.vinny` package hashes remain in vault history.

# v2.2.7 simple flat GUI, high-fps logo and golden orb - 2026-11-26

- **user direction:** "remove the black hole shit. we will just make the UI more simplistic", then "remove the black hole, make every theme simplistic, make the text for the theme names a little bigger. make the motion graphics for the orb and logo very nice and high fps. replace the orb buffer logo with what i gave u."
- **black hole removed:** `drawSpace()`, `m_space`, `hash01()`, nebula/stars/disk/photon-ring/orbit code and the `m_glow` decorative rings are gone. The shell is now one flat plate plus one flat card, with a flat sidebar row layout and a simple active-tab bar; every theme uses the same geometry and only its saved colors change.
- **orb:** the sidebar buffer emblem is now the supplied `jump_orb_only.html` golden jump orb: pulsing 10px ring, bright highlight ring, warm core, radial glow, two continuous ripples and 16 deterministic orbiting sparks. Tapping the buffer triggers the reference's click burst (`m_orbBurst`); off state dims the same orb instead of swapping to an unrelated icon.
- **logo:** still the animated 30-frame Dynamic D with no halo, but `setDelayPerUnit` is `0.0167f` so the existing frames loop at 60 fps (30 frames / 0.5s).
- **themes:** swatches are flat tiles with a slim primary-color bar and larger names (`0.48 -> 0.60` before width fitting). Theme switching still preserves `2.2.7` and all saved settings.
- **unchanged:** vertical sidebar order, small labels (`FONT_SCALE_BOOST 0.70`), identity, five credits, gameplay constants/hooks, stages, settings keys, saved data and cube behavior. No version bump.
- **build:** `0BBB4B9EF369A75C30D76A3E4BCEBB7A5B15C0D7679EC7A7A9464A8FE5BA5255`, deflate, build hash == installed hash; clean `Geode 2026-11-26 22.44.30.log` with silent 404 version check. This was the last installed `dynamic.vinny` build before the GitHub release changed the ID to `dynamic`.

# v2.2.7 black hole visibility and UI density pass - 2026-11-26

- **user feedback:** "the black hole can barely be seen, and the UI text and stuff is to big, everything looks cluttered." The first ADR-029 build proved the new shell loaded, but the space layer was buried under opaque deck/card plates and the shipped `ui-font-scale` default (`1.5`) made every label oversized.
- **black hole:** the event horizon grew from `HR 44` to `62`, moved nearer the visual centre (`x 430 -> 470`), and the disk/photon ring/nebula/star alphas and particle counts were raised. The disk now uses 620 points, 180 photon-ring points, 220 stars and brighter theme-blended colours.
- **legibility:** the deck/card fills were reduced (`shell 208 -> 112`, `card 186 -> 104`, shadow layers softened) so the animation reads through the panel. `FONT_SCALE_BOOST` is now `0.70`, bringing the effective default font scale from `1.5` to about `1.05` without changing the saved setting schema.
- **layering:** the space node now sits above the shell but below every control plate, sidebar, tab indicator and label, so the black hole is no longer buried under opaque cards while all hit targets stay clickable.
- **unchanged:** identity, version `2.2.7`, gameplay constants/hooks, stages, settings keys, saved data, five credits, 30-frame logo and cube behavior. No version bump.

# v2.2.7 black hole vertical GUI - 2026-11-26

- **new GUI style:** the old 700px control surface is now the main pane of a wider frame. A `154px` vertical sidebar sits to its left, with the visible tabs `stage -> modes -> straight -> log -> scan -> settings` and the orb/buffer toggle at the bottom. Every pre-existing control, hit rect and tutorial target inside the main pane keeps its original coordinate space.
- **animated space background:** `drawSpace(float dt)` redraws a theme-tinted starfield, nebula glow, accretion disk, photon ring, event horizon and two approaching/orbiting input particles each frame from `animTick()`. The disk palette follows the reference, but the final colours blend with the selected Dynamic theme, so changing themes changes the space and nebula around the black hole.
- **identity preserved:** the animated 30-frame Dynamic D logo and wordmark move into the sidebar header with no white halo. The status/orb control moves to the sidebar bottom and keeps the existing `BufferToggle` behavior; its emblem is a concentric orb with an orbiting spark.
- **rejected details removed:** the horizontal tab band, left-edge timing slashes, Play/Read/Tune dividers and `current`/section plates/captions are no longer drawn. The vertical indicator animates between nav slots with the diamond timing pip.
- **unchanged:** identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0`, version `2.2.7`, all gameplay constants/hooks, stages, settings, saved data, five Cyrillic credits and cube behavior. No version bump.

# v2.2.7 UI format pass - 2026-11-26

- **format first:** the user clarified that the current decorative work is a detail and the layout must be perfect first: "i mean the format of the UI mostly. not the details we will focus on details after format is perfect". This pass covers all three requested format pieces: overall proportions/spacing, tab structure/order, and control grouping inside tabs.
- **tab order:** internal indices stay `0 log / 1 stage / 2 settings / 3 modes / 4 straight / 5 scan`, but the visible rail is now `stage -> modes -> straight -> log -> scan -> settings` through `TAB_DISPLAY_ORDER`. Two stronger dividers split the rail into Play, Read and Tune. Labels, hit tests and the animated indicator all follow the visible order; tutorials and gameplay still use the stable internal indices.
- **proportions:** the card is taller (`CARD_TOP` 350 -> 366, `CARD_BOTTOM` 76 -> 70), the tab band moves up (`TAB_LABEL_Y` 388 -> 398, `TAB_UNDERLINE_Y` 366 -> 376), and every pane's rows move with it (stage, log, settings, scan, modes, straight, chips) so spacing stays balanced rather than squeezing the taller card.
- **control grouping:** every tab now gets non-interactive section plates with a short accent rail: stage splits Current / Track / Tick offsets, modes gets one tile field, straight splits Assist from Dials, log gets an Activity field, scan splits Current level / Structure / Remembered scans, and settings splits Themes / Panel rows / Config tools. Captions appear only where they do not crowd a control. No hit rect, setting value or visual control moved.
- **unchanged:** identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0`, version `2.2.7`, the 30-frame logo, five Cyrillic credits, no white logo halo, silent update check, all gameplay constants/hooks and cube behavior. No version bump.

# v2.2.7 final UI identity pass - 2026-11-26

- **distinct Dynamic silhouette:** the deck shell, glow/shadow layers, header/footer bands, card and tab rail now use the new `drawCutRect()` plate with clipped top-left / bottom-right corners instead of generic rounded rectangles. This is the deliberate "unique and different" look the user asked for; colours still follow the selected theme.
- **new signature details:** a nine-step alternating timing rail runs down the free left edge, the wordmark underline is a three-stage kinetic rule, and the active tab is a notched plate with a diamond timing pip instead of a pill/circle.
- **unchanged:** every tab, hit rect, setting, layout target, the 30-frame logo, five Cyrillic credits, no white logo halo, identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0`, version `2.2.7`, gameplay and cube behavior. No version bump.

# v2.2.7 silent version check - 2026-11-26

- **no more "version check failed" in the in-game log:** the Dynamic update URLs point at `github.com/dynamic-gd/dynamic`, which is not published yet and returns HTTP 404. Missing, empty or blank `version.txt` responses now write only a `dynamic version check skipped ...` line to the Geode console instead of calling `say()`, so the UI log stays clean on launch.
- **future updates still work:** once a real Dynamic `version.txt` is published, the existing out-of-date notice, release tag check and in-game install path run unchanged.
- **unchanged:** identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0`, version `2.2.7`, UI, logo, credits, gameplay and cube behavior. No version bump.

# v2.2.7 metadata naming cleanup - 2026-11-26

- **all active metadata is Dynamic:** verified `mod.json` (`dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0` / `2.2.7`), `about.md`, README, CMake project/target names, in-game UI strings, terminal filenames and update URLs. No Afterflow name remains in active mod metadata.
- Renamed the last stray active names: `LICENSE` now reads `prsevend (Dynamic)`, and `tools/check-geode-compression.py`, `tools/check-geode-platforms.py` and `tools/check-ios-linkage.py` use `dynamic.vinny`, `dynamic.vinny.*` platform binaries and `dynamic.vinny.ios.dylib`. Historical changelog entries, backups, old build logs and the vault folder display name intentionally keep the old name.
- Repackaged so the bundled `changelog.md` matches source. No gameplay, UI, logo, credits, identity or version changes.

# v2.2.7 logo glow removed - 2026-11-26

- **white glow around the logo removed:** the four accent ring circles drawn behind the floating launcher and deck-header marks and the five additive tinted logo copies on the pause-button face are gone. `makeGlowingLogo()` is now `makeLogoNode()` and holds only the animated sprite, and the pause button draws just the mark. The 30-frame logo art and animation are untouched.
- **unchanged:** identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0` / `2.2.7`, the five Russian-name credits, all hit targets, settings and gameplay/cube behavior. No version bump.

# v2.2.7 high-fps motion, full UI rework and new credits - 2026-11-26

- **high-fps motion:** the kinetic Dynamic logo is now 30 frames `resources/dynamic-logo-f00.png` .. `dynamic-logo-f29.png` at 0.0333s per frame instead of 12 frames at 0.075s. The loop is visibly smoother while the static Geode icon stays the matching frame 00. All other logo art and animation surfaces (header, glow copies, mobile launcher, pause button) are unchanged.
- **whole UI rework:** the control deck was restyled end to end without moving a single hit target. The shell now has layered shadow, glass header/footer bands, hairline accent rails, a segmented tab rail and a brighter card surface. Buttons, steppers, the animated tab indicator, status pill, log highlight and credits button all use the same new accent treatment. Every tab, setting, layout constant and gameplay hook is untouched.
- **new credits:** the old three-row panel was replaced with a five-row card: `ВинниGD`, `Кисель`, `Мортис`, `Тёмный дракон`, `Славик404`. The user then said **"on credits remove 6 7 and 8"**, so the three extra community rows are gone and the card was tightened to `616x330` with larger rows. Names and roles use a Cyrillic-capable `CCLabelTTF` Arial path because the bundled Sora BMFonts do not contain Cyrillic glyphs.
- **unchanged:** identity `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0` / `2.2.7`, gameplay constants, cube behavior, no spam guard, no recreation, no version bump.

# v2.2.7 Dynamic rename + new logo - 2026-11-26

- **full rename:** the real mod is now `dynamic.vinny` / `Dynamic` / `vinny` / `@v1nny0` / `2.2.7`. The UI, tutorial, credits, logs, class IDs, resource names, terminal/runtime filenames, update URLs, CMake target, installed package, save/settings directory and loader keys were all renamed. No version bump.
- **everywhere in the UI:** floating and pause buttons are now the "Dynamic" button, the overlay/log prefix is `dynamic:`, and every resource is `dynamic-*`. The old `afterflow.vinny.geode` was removed and both extracted caches were purged.
- **migration:** `saved.json` and `settings.json` were moved from `afterflow.vinny` to `dynamic.vinny`; loader keys became `should-load-dynamic.vinny` and `is-pinned-dynamic.vinny`.
- **new logo:** the supplied `logo.svg` first became the Geode icon and UI sprite; after the user said it still looked like Afterflow, it was replaced with a unique kinetic D: rotating broken orbit arcs, breathing speed streaks, a chromatic ribbon D, a moving timing pulse and a comet trail. The matching static frame is `logo.png` / `resources/dynamic-logo.png` (SHA-256 `219816BE655117FEABB4AA76B9981361A251E6914F538C18F75F13233713C910`); `resources/dynamic-logo.svg` is the new vector source.
- **animated UI:** 12 frames `resources/dynamic-logo-f00.png` .. `dynamic-logo-f11.png` (frame 00 SHA-256 `9C31A86E199299A9E82ABB601C741BB7A0A97BCB1FA68C76E7856553D69377E6`) are loaded by `animateDynamicLogo()` into a looping `CCAnimation` at 0.075s per frame. The header, glow copies, mobile launcher and pause button all animate; gameplay is untouched.
- **build:** current installed `dynamic.vinny.geode` (build hash == installed hash; SHA-256 recorded in the vault's `01 Project/Build and Install`), bundling all 12 animated frames and a `changelog.md` matching the source. Startup `Geode 2026-11-26 20.28.06.log` shows `dynamic.vinny v2.2.7`, three RingObject hooks, `dynamic: load complete`, overlay attached and menu ready, no new errors.
- **unchanged:** gameplay fixes from the five-fix hotfix and orb pass 2, cube behavior, no spam guard, no recreation, no version bump. Feel and the animated logo's in-game look still need a playtest.

# v2.2.7 gameplay hotfix - 2026-11-26

- **high-CPS wave:** rapid taps are now judged as a complete two-sided gesture. the sim uses the learned press->release hold and the observed release->press gap for the press, then swaps them for the release, instead of treating a wave press as "hold forever" and a release as "fall forever". logs tag this as `rapid {hold}t/{gap}t`.
- **ship straight fly is now visible:** it judges its own 16t window instead of inheriting the 48t crash-search gate, and its nudge cap no longer collapses to the emergency 4t cap. a dial at S90 now logs a 7t cap (max 8t at S100), with both the straight cap and ship cap printed on every nudge.
- **in-range orb clicks get real retiming:** a press already inside a ring is no longer passed raw on the first usable tick. the helper sweeps the same-ring safe band and moves the click to the middle of the longest contiguous run. logs read `[orb] in-range: press now ... band ... picked +... landing ...`.
- **[fix] ghost sims no longer flash real orbs:** `RingObject::triggerActivated` and `RingObject::powerOnObject` are suppressed while `state.isSimulating`, alongside the existing shine/circle suppression. a simulated contact far from the real player cannot animate the orb anymore.
- **spam guard removed completely:** no toggle, state fields, saved key, banner, hit target, icon, or config export/import fields remain. fast inputs are no longer thrown raw past the buffer. rapid counting survives only for the pre-existing ufo/ball/swing one-tick compaction.
- **live save:** removed the stale `spam-guard` key and turned `orb-buffer` back on. mod id, name, developer and version stay `afterflow.vinny` / `Afterflow` / `vinny` / `2.2.7`.
- **orb pass 2 - in-range clicks that only survived part of the horizon are no longer called `kept raw`.** the live log proved the first pass still let clicks die after `47t of 130t`; the helper now sweeps the full orb reach and accepts a same-ring band that either clears the horizon or buys at least `8t`, then picks the middle of the longest band. logs still read `[orb] in-range: ... band ... picked +... landing ...`.
- **orb pass 2 - every mode and every clickable orb type.** a ring actually under the click is no longer rejected for wave/ship/grounded; early presses keep the old mode gates. all `RingObject` types use the same simulated press, including `DashRingObject` (the old dash-ring skip is gone). gravity orbs in wave and dash orbs in ship now reach the helper when touched.
- **build:** `9C264E9A97BAB97E800680A6BDF20C79DDD551B0894A82DC971202E0623BE841` (same SHA build and installed package). supersedes `0EA43131...31107`. startup log `Geode 2026-11-26 19.50.44.log` shows v2.2.7 loaded with all three RingObject hooks and `load complete`; gameplay feel still needs a playtest.

# v2.2.7
- **rolled back to 2.2.5.** the whole gameplay side is the 2.2.5 code again. the prediction, the ghost players, the click buffer, the wave handling, the budgets and the sweep are all the exact 2.2.5 functions, so it plays like 2.2.5 again.
- **kept the modes tab.** the panel has a fourth tab now, one tile per game mode with the mode icon, the current ticks under it, `-5` / `+5` on the top row, `-` / `+` on the bottom row, tap the tile to put that mode back on auto and `all auto` to clear every mode. wave still follows the stage ticks.
- a mode set to auto uses the stage ticks, so nothing changes unless you tune a mode.
- **mobile.** the overlay is no longer built while a scene transition is running, the panel waits for the real scene the same way 2.2.6 did, and it hides itself while a geometry dash popup is open so its touches stop fighting the alert.
- **mobile.** buffer players that fail to create now turn buffering off for that level instead of walking a null pointer, the collision logs and the ghost player are null checked, and the ios build no longer overrides the player effect calls ios does not have.
- **update check.** version.txt alone no longer triggers the update popup, the release tag for that version has to exist too, so a bumped version.txt with nothing published yet waits until the tag is pushed.
- bumped version to `2.2.7` (mod.json, version.txt, CMakeLists.txt).

# v2.2.4

- **fix: ios crash at the end of the loading bar.** the panel was being built from `CCDirector::willSwitchToScene`, i.e. in the middle of the loading screen -> menu transition, and allocating fonts and sprites there is what kills ios. the overlay is now built on the next main thread tick, against whichever scene is running by then.
- [fix] the stage strip no longer dereferences a label that failed to create, and it is bounds checked against the stage label array.
- [fix] the version check no longer runs inside `$on_mod(Loaded)`; it fires once the menu layer is up. `setMouseEnabled` is desktop only now.
- added startup breadcrumbs to the geode log (`load start`, `load complete`, `building overlay`, `overlay attached`, `menu ready`) so a crash on a platform we cannot debug locally can be pinned to a stage.
- added `tools/check-ios-linkage.py`, which checks a packaged `.geode`'s ios slice against a geode ios release.
- [build] the combined package is forced back to deflate. the `combine` action started defaulting to `--zstd` and the geode ios loader has no zstd, so the package extracted to nothing and the game said "no binary could be found for current platform". the action is pinned and a check step now fails the build if a package is not deflate.
- you can drag the pause menu af button anywhere on mobile now, and the button glow follows the logo instead of drawing a circle around it.

# v2.4.1

- **mobile pause menu button**: android and ios now get an AF button in the pause menu, directly under the settings button and the same size as it. tap it to open the control panel.
- the floating draggable af button is now off by default on mobile; turn it back on with the new "floating af button" setting.
- keybind and terminal settings are now windows/mac only.
- [build] workflow now builds android32, android64 and ios, and still packages one combined `.geode` if ios fails.

# v2.4.0

- **auto-update in game**: the out-of-date popup now downloads, verifies and installs the new `.geode` itself, then restarts the game. no more browser round-trip.
- the update download shows a live progress panel with percentage, MB downloaded / MB total and current speed in MB/s.
- downloads always resolve `releases/latest/download/afterflow.prsevend.geode`, so the updater no longer depends on the release tag naming scheme.
- update installs use a safe write (temp file + rename), so an interrupted download can no longer leave a corrupt `.geode` behind.
- added a 120s transfer timeout to the update download so a dead connection cannot strand the progress panel.
- **config import / export**: the settings tab can copy the current gameplay tuning to the clipboard as JSON and paste it back, with clamping and a status readout.
- **game mode icons**: the settings tab shows the cube, ship, ball, ufo, wave, robot, spider and swing icons in your own colours, centred in their own row with a rounded backdrop. the icon for the mode you are currently in is highlighted, the rest dim.
- [fix] game mode icons no longer render inside the theme colour swatches.
- [fix] the newest log line is marked with a full-width accent row instead of a 3px vertical bar that looked like a text cursor.
- [fix] panel glow reduced from 7 rings at 8 alpha to 5 rings at 3 alpha, and the drop shadow softened from 120 to 72 alpha.
- [fix] panel shell is no longer crushed to near-black: the tint multipliers moved from `0.62/0.70` to `0.88/0.92`.
- [fix] smoother rounded corners everywhere (`SEGMENTS` 10 -> 24).
- [fix] `CMakeLists.txt` version was stuck at `2.3.0` while `mod.json` and `version.txt` said `2.4.0`.
- [build] a post-build step now clears geode's extracted-mod cache so a rebuild can never silently load a stale dll again. disable with `-DAFTERFLOW_PURGE_UNZIPPED=OFF`.

# v2.3.0

- redesigned the in-game panel: compact header, stage pills, all-mode/wave stat readouts, striped live log, translucent cards and softer pill buttons.
- new **ui customization** settings: primary color, secondary color, panel scale and font scale. changes apply live while the panel is open.
- [fix] windows builds with clang-cl and ninja again; msvc-only flags and apple architectures are no longer applied to every platform.
- [fix] the packaged `.geode` now includes the bundled fonts and their license.

# v2.2.0

- new custom in-game control panel on **f9** with stage arrows, stage dots, a live log, copy log, close and a terminal shortcut.
- the panel pauses the game while open and resumes when it is closed.
- added android and ios targets. mobile gets a draggable **af** button that opens the same panel, and its position is saved between sessions.
- the panel uses bundled sora fonts, with `bigFont.fnt` as a fallback.
- f9 now opens the control panel instead of the terminal directly. use the **terminal** button in the panel to open the log window.
- the live log keeps recent messages so the panel can show them and copy them to the clipboard.

# v2.1.6

- f6 now cycles stages from the windows powershell terminal and the linux kitty terminal. windows handles f6 directly in the log viewer; kitty maps f6 to a cycle helper and keeps the raw-key fallback.

# v2.1.5

- stage cycle feedback and terminal errors now go to the terminal log only. afterflow no longer shows geode notifications; open the windows powershell terminal or kitty terminal to see the selected stage.

# v2.1.4

- f6 now cycles stages outside a level too. a notification shows the selected stage while in a menu or editor.
- [fix] linux/proton terminal not opening after v2.1.2 on some compositors such as cachyos/hyprland. the watcher now finds the wayland or x11 session itself instead of depending on a process named hyprland.
- [fix] macos package now includes the native dylib, so geode no longer reports `unable to find platform binary under the name "afterflow.prsevend.dylib"`.
- linux users must update `afterflow-terminal-watch.sh` and `afterflow-terminal.service` along with the mod.

# v2.1.3

- [fix] windows terminal not showing.

# v2.1.2

macos support

# v2.1.1

- [fix] terminal runtime files are now per-user (`/tmp/afterflow-<user>`), so switching Linux accounts no longer leaves stale triggers, close files or logs that stop the terminal from opening or make kitty open and close in a loop.
- the terminal watcher ignores a trigger that existed before it started, so a stale trigger from a previous session cant reopen kitty on login
- [fix] terminal not showing up when clicked on custom set userkeybind or f9.

# v2.1.0

- f6 and f9 are now real geode keybinds instead of hard coded non-changable set configs (i realized that people with 60% kbs dont have function keys so i "fixed" this.) 
- first install comes with a tutorial that tells them how to change the keybinds for the terminal and the cycling
- the terminal toggle is no longer a hardcoded keyboard hook; it listens for the keybind setting press.

# v2.0.5

- fixed ghost predictions and death effects running while paused

# v2.0.4

- close terminal via F9 (Linux & Windows)
- [afterflow] terminal

# v2.0.3

- fix visual (dash) orbs bugging
- added 10f / 5f @ 60fps (stage 7)

# v2.0.2

- added the version check. reads `version.txt` on launch and when a level starts, at most every 10 minutes.
- if it differs from the installed version, the console and terminal say `out of date` with the release link, and the menu shows a `dismiss` / `update` popup. `update` opens your default browser.

# v2.0.1

- new purple diamond icon.
- terminal, window class and log paths renamed.
- f9 opens kitty on linux/proton or cmd on windows.
- f6 works while the terminal is focused.

# v2.0.0

- whole undetected revamp for afterflow (was previously afterglow/moonsky)

# v1.0.0

inital release