# Timer Visual Upgrade Plan

## Goal

Upgrade the Qualia display in `device_code/production.ino` so the timer UI matches the new concept direction:

- new card-by-card color palette
- animated graphics tied to timer themes
- Racing Sans One instead of the current pixel-style built-in font
- a polished two-timer presentation as the primary visual target

The goal is to improve visuals without breaking the existing timer, BLE, demo-mode, and sound behavior.

## Current Project Structure Insights

### Top level

- `device_code/` is the important firmware area for this task.
- `timer_companion_app/` is a separate Flutter app and is not part of the firmware currently flashed to the Qualia.
- the repository root also contains CAD exports, case files, screenshots, and older experiments that are unrelated to the live timer UI path

### Device firmware

- `device_code/production.ino` is the active Qualia firmware you are loading now. It contains display setup, BLE peripheral logic, timer state, sound effects, demo mode, and all screen rendering in one large sketch.
- `device_code/production_nicla.ino` is the separate Nicla Voice firmware. UI work on the Qualia should not require protocol changes unless timer names/categories are expanded.
- `device_code/command_protocol.h` is the small shared command contract between voice/demo parsing and timer execution.
- `device_code/main.ino` looks like an earlier UI prototype. It is useful as reference, but it is not the current production target.
- `device_code/Mylene Timer/` is a PlatformIO scaffold, but it does not appear to be the main build path right now since the current workflow is Arduino IDE plus `production.ino`.

### Visual assets already in repo

- `device_code/assets/` contains the source GIFs:
  - `coffee.gif`
  - `books.gif`
  - `mixy.gif`
  - `dumbell.gif`
- `device_code/font/` contains:
  - `RacingSansOne-Regular.ttf`
  - `RacingSansOne_Regular20pt7b.h`

### Useful reference outside firmware

- `timer_companion_app/lib/screens/homescreen.dart` already uses Racing Sans One and a radial gauge. It is not the production device UI, but it is a useful style reference inside the repo.

## What The Current Firmware Is Doing

Based on `device_code/production.ino`:

- the display is initialized as a 320x960 panel and then rotated into a 960x320 landscape layout
- the timer UI is drawn directly with `Arduino_GFX` primitives
- the current palette is hard-coded and mostly blue-based
- the ring graphics are procedural, not image-based
- timer names and times are drawn with the built-in font using `setTextSize(...)`
- the two-timer split layout already exists, using two 480-pixel-wide panels
- the one-timer and three-timer layouts also already exist
- demo mode is already built in, which is useful for visual testing without voice input

## Most Relevant Code Hotspots

These are the parts of `device_code/production.ino` that matter most for the visual upgrade:

- `drawNoTimersScreen()` around lines 731-757
  - current idle screen
- `setup()` around lines 769-875
  - display boot, rotation, and initial screen draw
- `renderTimers()` around lines 878-997
  - current 1-timer, 2-timer, and 3-timer layouts
- per-second text refresh around lines 1082-1128
  - current time redraw logic
- ring animation update around lines 1131-1204
  - current animated countdown rendering path

## Technical Findings That Matter For The Upgrade

### 1. Font swap is not just a one-line change

The current firmware relies on the built-in bitmap font and integer `setTextSize(...)` scaling. Moving to Racing Sans One means the text layout should be reworked around measured bounds instead of fixed character-width assumptions.

Practical impact:

- timer name and timer value positions will need to be recalculated
- partial updates should clear and redraw bounded text regions explicitly
- custom font rendering should be treated as a real layout change, not a simple font include

### 2. The animation assets are too large to use raw as-is

Current asset sizes:

- `books.gif`: 320 x 480
- `coffee.gif`: 600 x 700
- `mixy.gif`: 600 x 600
- `dumbell.gif`: 800 x 750

Those are much larger than the area each graphic will occupy in a 480 x 320 half-panel. They should be resized and converted into device-friendly animation assets before use.

### 3. There is no existing GIF playback pipeline in production firmware

I did not find any active filesystem mounting, GIF decoding, or image-decoder usage wired into `device_code/production.ino`. That means animated graphics will need either:

- an on-device decode path added intentionally, or
- an offline conversion pipeline that turns the GIFs into device-ready frame data

### 4. The current ring update path already does expensive per-frame work

The ring animation logic currently reinitializes ring lookup data during animation updates. That is workable today, but adding animated graphics on top of it will increase the chance of dropped frames or sluggish redraws.

This is one of the main reasons I would refactor the render path before trying to layer in GIF playback.

### 5. The repo structure can make Arduino IDE maintenance harder

`device_code/` contains many independent `.ino` sketches in one folder. That is fine as a working archive, but it is not ideal once the production UI starts gaining helper tabs, generated assets, and font files.

For maintainability, the active Qualia firmware should eventually live in its own dedicated sketch directory.

## Recommended Implementation Strategy

### Phase 1: Stabilize the active firmware path

Recommended first move:

- keep `production.ino` as the source of truth for now
- optionally move it into a dedicated sketch folder before the refactor grows
- avoid touching BLE command handling and timer logic unless needed

Why:

- the visual work is already significant
- separating UI work from timer logic will reduce regression risk

### Phase 2: Introduce a UI theme layer

Create a small theme table for timer categories. Each theme should define:

- background card color
- empty ring color
- ring gradient end color
- text color
- asset id / animation id

Example mapping based on the concept image:

- Break -> coffee
- Homework -> books
- Baking -> mixy
- Exercise -> dumbell

This should replace the current random ring-color assignment for named timers, at least for the themed timer names already supported by the voice system.

### Phase 3: Refactor rendering around reusable timer panels

The rendering code should move toward a `drawTimerPanel(...)` style helper that knows how to draw one timer card, regardless of whether it is in 1-up, 2-up, or 3-up mode.

Each panel should render in layers:

1. background card
2. animated graphic
3. timer name
4. timer value
5. countdown ring

Benefits:

- less duplicated layout math
- easier to tune the two-timer layout first
- easier to keep single and three-timer layouts working afterward

### Phase 4: Replace the built-in font with Racing Sans One

Use the existing `device_code/font/RacingSansOne_Regular20pt7b.h` as the starting point, but expect that one size may not be enough for all layouts.

Recommended approach:

- test the existing 20pt header first
- likely generate at least one additional smaller size for the 2-timer and 3-timer layouts
- use explicit bounding boxes for time and title redraws
- stop relying on fixed-width text placement assumptions

Important note:

- the current per-second text update code prints new time values directly onto the screen
- with a custom font, that should be replaced by "clear region, then redraw text"

### Phase 5: Add an asset pipeline before trying full animation playback

I would not try to play the source GIFs directly in the first implementation pass.

Recommended path:

- preprocess each GIF offline
- resize/crop for the actual on-screen target area
- export a compact animation format for the device

Best practical options:

- Option A: convert each animation into a short set of RGB565 frames embedded in headers
- Option B: convert each animation into reduced frame assets stored in flash and streamed at runtime
- Option C: if performance gets tight, use a static first frame for v1 and add animation in v2

My recommendation:

- use preprocessed reduced-size frame data rather than raw GIF decode on-device

That is the lowest-risk path for the current firmware structure.

### Phase 6: Optimize redraw behavior before layering in animation

Before or during animation integration:

- cache ring LUTs per scale instead of rebuilding them repeatedly
- track dirty regions for time text, animated asset area, and ring area separately
- only redraw changed regions on second-by-second updates
- keep full-screen redraws for layout changes only

This is the best chance of keeping the UI smooth once animated graphics are present.

### Phase 7: Use two-timer mode as the design anchor

Since the concept image is for the two-timer layout, I would implement in this order:

1. two timers
2. one timer
3. three timers

That keeps the initial design pass focused while still preserving the current product behavior.

For the first release:

- make the two-timer view match the concept most closely
- keep one-timer and three-timer modes functional, even if they are slightly less polished at first

## Suggested File-Level Changes

If the refactor stays in the current structure, these are the likely touch points:

- `device_code/production.ino`
  - move theme constants out of ad hoc globals
  - replace the current text layout code
  - add panel-based drawing helpers
  - add explicit redraw regions

- `device_code/font/`
  - keep the TTF as the source asset
  - add additional generated font headers if needed

- `device_code/assets/`
  - keep original GIFs here as source material only

- new generated asset area, for example:
  - `device_code/generated/`
  - or `device_code/assets_processed/`

Possible helper files if you want to reduce the size of `production.ino`:

- `device_code/ui_theme.h`
- `device_code/ui_layout.h`
- `device_code/ui_render.ino`
- `device_code/ui_assets.h`

## Risks And Constraints

- custom font rendering will change text measurement and partial redraw behavior
- raw GIF playback may be too expensive without preprocessing
- current ring animation work may already consume enough time to compete with sprite animation
- all visual changes still need to coexist with BLE, demo mode, alarm states, and countdown timing
- the asset folder is currently untracked in git, so asset management should be cleaned up before the visuals depend on it

## Recommended Order Of Work

1. Lock the target two-timer layout and define final panel geometry.
2. Add a theme mapping for the supported timer names.
3. Integrate Racing Sans One and replace the current text placement logic.
4. Refactor panel drawing into reusable helpers.
5. Optimize ring redraw behavior and cache repeated math.
6. Preprocess the GIFs into device-ready reduced assets.
7. Add animated asset playback to the two-timer layout.
8. Apply the same visual system to one-timer and three-timer layouts.
9. Run demo-mode validation plus real BLE command testing.

## Recommended First Implementation Pass

If I were doing the implementation next, I would start with this exact scope:

- keep all existing timer behavior intact
- refactor only the display code in `production.ino`
- implement the new palette and Racing Sans One first
- make the two-timer layout match the concept image
- use static graphics first if needed
- add true animation only after redraw performance is acceptable

That sequence keeps the work realistic and reduces the chance of mixing visual refactors with timing regressions.
