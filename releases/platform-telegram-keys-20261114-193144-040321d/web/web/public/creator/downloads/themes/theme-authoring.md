# VLTONE theme authoring, schema 2

A theme describes existing controls, authored windows, parameter bindings and button commands. Musical project time and pitch belong to the project. Devices have their own protected factory context. Host headers and generic host parameters use the application context.

## Free canvases and user panels

A layout with `"scope": "descendants"` addresses controls across nested Qt layouts. The original widget moves to the canvas and keeps its signal connections and accessibility object. A private slot records where to return it on revert. `canvasHeight` is in logical pixels. A taller track header uses the same display height as its timeline lane. Project track heights stay in the project. `absolute: true` removes one item from a row, column or grid flow so it can have its own X and Y. A signed integer `z` sets stacking order independently of a flow or neighbour anchor. Native percentages, offsets, anchored groups and width variants work on the same canvas.

Set an item's `fromWorkspace` flag to transfer a unique application widget into another canvas or authored window. Each imported ID has one destination. Runtime checks reject ambiguous repeated instances, device controls, editors, cyclic parenting and transient dialogs as sources. A track template retains its track binding. A header toggle can use `selectedTrack.solo` to act on the currently selected track.

Theme packages embed their generator source and declarative panels. A command implemented by a separate scripting extension keeps its SDK identity `script.<package>/<command>`. Distribute that extension alongside a theme that uses it. Recovery startup suppresses automatic script windows.

The optional `panels` object is keyed by `user.<id>`. A panel has `title`, `width`, `height`, `host` and an ordered `controls` array. Supported hosts include `window`, `dock.left`, `dock.right`, `dock.top`, `dock.bottom` and a unique inline host such as `workspace.header`. A control is named `<panel>.<id>`. Its type is `button`, `toggle`, `number`, `slider`, `text`, `choice` or `label`. Give a button a registered `command`. Bind compatible controls to `transport.tempo`, `selectedTrack.solo`, `selectedTrack.mute`, `selectedTrack.pan` or `selectedTrack.volume`. A control without a project binding uses a per-profile setting. The optional `setting` shares that name within the panel. The package contains the authored definition and defaults. The recipient supplies their own personal values.

Both the visual editor and the isolated Lua generator produce this model. Visual edits to generated panels are stored in `panelOverrides`. Custom windows are applied with layout, including on layout-only import and preview rollback. The scripting SDK supplies event handlers, drawing and executable commands. Script widget IDs use `script.<package>.<window>.<control>` and survive reruns and project changes. Unnamed controls use their position in the widget description. Give a control an explicit ID when rearranging the Lua source. See the [three-row workbench generator](../../resources/themes/workbench/theme.lua) and [interactive scripting example](../../sdk/scripting/examples/interactive-workbench/main.lua).

## Author workflow

Authors can still bind their own button or shortcut to the existing `transport.tapTempo` command. Two taps set the tempo and subsequent taps average up to four beat intervals.

Open **Appearance → Theme editor**. **New from example** creates a private draft
of Glass, Compact, Amber Console or the three-row Workbench. The installed example is never edited.
Choose an element in the tree or canvas. Expand buttons, knobs and faders to
inspect their parts. The inspector shows the source of each inherited property.
Resetting a property removes its override. The scope selector switches between
one semantic element and its shared component type.

Drag a selection to move it; drag its lower-right handle to resize. Ctrl selects
multiple elements. Arrow keys move by one logical pixel, Shift by ten, Alt
resizes. The Arrange menu provides rows, columns, grids, alignment, grouping,
visibility and stacking order. Numerical fields provide the same geometry.
Theme Undo/Redo is independent of project Undo. Interact toggles a demonstration
button, drags a fader or knob and accepts wheel and arrow keys; it does not send
audio commands. The editor is modeless, so the real workspace remains usable.
**Preview in application** temporarily publishes the theme for testing real
controls; Revert or closing the editor restores the previous appearance and
workspace. **Apply** publishes the selected appearance/layout choices.

Appearance is selected initially; layout is opt-in. Loading an appearance must
not replace a musician's workspace. A layout change waits until pointer capture
ends. The resulting QWidget rectangles are also the input and accessibility
rectangles; there is no separate invisible set of button hit boxes.

The Graphic layers dialog imports PNG, SVG and fonts, and edits layer ordering,
bounds, paths, opacity, colour, sprite frames and value-dependent transforms.
Imported fonts receive an alias usable in `fontFamily`. Save copy copies the
source and referenced artwork/fonts into the selected folder. **Export theme**
writes a portable `.vlttheme` from the current validated draft without applying
it. The package includes its own layouts, generator, overrides and assets even
when the musician has chosen to keep a different workspace. Existing library
capture/export remains available for the currently running interface.

## Document

```json
{
  "schemaVersion": 2,
  "name": "My console",
  "defaults": { "material": "flat", "radius": 3 },
  "components": {
    "button": {
      "foreground": "#f0ece0",
      "states": { "checked": { "background": "#916b32" } }
    }
  },
  "elements": { "transport.play": { "borderWidth": 2 } },
  "overrides": {},
  "layouts": {},
  "assets": {},
  "fonts": {}
}
```

Inheritance is defaults → component → element → user component overrides → user
element overrides. State rules participate at each level; combinations such as
`checked+hover` are more specific than `hover`. Equal-specificity combinations
use lexical order. Normal properties belong directly in the rule; state keys
are `hover`, `pressed`, `checked`, `focused`, `disabled`, joined with `+`.

Visual properties are `material`, `background`, `foreground`, `border`,
`endColor`, `radius`, `borderWidth`, `opacity`, `fontFamily`, `fontSize`,
`fontWeight`, `padding`, `metrics`, `palette` and `layers`. Colours use QColor
strings, including `#AARRGGBB`. Font sizes and lengths are logical pixels.
`padding` is a number or `[left, top, right, bottom]`. Native metrics are
`indicatorWidth`, `indicatorHeight`, `sliderLength`, `sliderThickness`,
`scrollBarExtent`, `frameWidth`, `tabSpacing`, and `iconSize`.

Materials: `glass` uses the factory glass painter, `flat` a solid surface,
`gradient` a vertical gradient, `layers` the authored layers without an automatic
background. Standard Qt controls use the same prepared styles via QProxyStyle.
Custom painters use `ThemeContext`, `paintThemeControl`, `paintThemeSurface`,
or `paintThemeElement`. Musical content templates include `timeline.clip` and
`pianoRoll.note`; their rectangles remain project-owned.

Layers draw in array order. Types: `rect`, `ellipse`, `line`, `path`, `text`,
`image`. `bounds` is `[x, y, width, height]` relative to the component. Common
properties are `fill`, `stroke`, `strokeWidth`, `radius`, `opacity`, `rotation`,
and `clip` (`rect` or `ellipse`). `origin: [x, y]` chooses a normalized
rotation pivot; the layer centre is used when omitted. Text supports `$label` and `$value`. `asset`
references the `assets` map. `frames` splits an image into vertical sprite
frames. `slices` specifies image edge widths for nine-slice drawing.
`valueRotation`, `valueBounds` and `valueFill` interpolate from a normalized
control value in native code; no Lua runs during drawing.

Paths are arrays of commands: `["M",x,y]`, `["L",x,y]`,
`["Q",cx,cy,x,y]`, `["C",c1x,c1y,c2x,c2y,x,y]`, `["Z"]`.
Image paths and font paths are relative to the document directory and must stay
inside it. `fonts` maps an author-selected alias to a TTF/OTF/TTC file; the
prepared style resolves the alias to the embedded family's actual name.

## Layout

`layouts` maps a container ID to a recipe. Items name existing child widgets or
internal parts. Part names include button `background/icon/label`, knob
`body/indicator/label`, fader `rail/cap/scale/label`. Fader movement along its rail
remains tied to its value; editing its rail changes the same travel used by input.

```json
{
  "mode": "row", "gap": 4, "padding": 3,
  "items": [
    { "id": "transport.stop", "width": 32 },
    { "id": "transport.play", "stretch": 1, "minWidth": 40 }
  ],
  "variants": [
    { "maxWidth": 240, "gap": 1 }
  ]
}
```

Modes are `row`, `column`, `grid`, `free`. Grid has `columns`. Lengths may be
numbers or percentages. Items support `x/y/width/height`, minimum and maximum
sizes, `visible`, `stretch`, and cross-axis `align` (`start/center/end/stretch`).
Free anchors are `top-left`, `top-right`, `bottom-left`, `bottom-right`, `center`.
`relativeTo` refers to an earlier sibling; `after/before/above/below` position
relative to that sibling. Requiring an earlier sibling prevents cycles.
Groups use a nested `layout` and do not introduce functional widgets.
`offsetX/offsetY/widthOffset/heightOffset` add logical-pixel adjustments after
resolving a length or anchor. The visual editor uses these for an existing recipe,
so dragging an anchored control preserves percentages and responsive variants.
A `null` entry in `layoutOverrides` disables a generated recipe without rewriting
its source. Reopening and export preserve the separate override layer.

Track templates have an outer `track.header` and inner `track.header.controls`;
`track.header.states` contains mute/solo/record/monitor where the track supports
them. The console example changes this inner template while native availability
rules continue to hide controls irrelevant to that track.

Variants use `minWidth/maxWidth/minHeight/maxHeight`. The last matching variant
wins over the base recipe. Every variant is compiled when the document is
validated, including variants not selected by the current window. Array order
also supplies widget stacking and keyboard tab order. Geometry is compiled into
immutable native trees; resize resolves these trees without parsing JSON.

`workspace` captures editor docking slots, splitter sizes, selected tabs and
floating editor windows. These affect existing workspace objects. Floating
positions go through the existing screen-constraining window placement API.

## Lua and external editing

Open a `.lua` file or use the Lua tab. A generator returns a schema-2 table:

```lua
return {
  schemaVersion = 2, name = "Generated compact",
  defaults = { material = "flat", radius = 2 },
  components = { button = { foreground = "#efefef" } },
  layouts = { ["transport.controls"] = {
    mode = "row", items = array {
      { id = "transport.stop", width = 32 },
      { id = "transport.play", width = 40 }
    }
  }}
}
```

`array{}` distinguishes an empty JSON array from an empty object. The generator
runs in `vlt_script_worker --theme`, with no host commands, file APIs, process
APIs or module loading. It has a 64 MiB allocation limit, a two-second execution
hook and a three-second host watchdog. Source is limited to 1 MiB. Errors retain
the last prepared preview and report the Lua source location.

Mouse edits on generated themes use `overrides`, `componentOverrides`,
`layoutOverrides`, `appearanceOverrides`, `assetOverrides` and `fontOverrides`. Regeneration preserves
these sections. The editor never rewrites Lua to reflect mouse operations.
JSON stores the generator's source and optional adjacent `file`. External JSON
and Lua changes are checked before replacing a clean draft. A dirty draft keeps
its edits and reports a conflict; Save copy preserves both versions. Save also
checks disk contents immediately, covering the interval before watcher delivery.

Imported packages carry `resourcePaths` so regenerated code can still refer to
its original filenames while installed resources use content-addressed names.
Opening or applying a package does not execute embedded Lua.

## Standard controls and materials

The document's `appearance` object includes the previous separate preferences:
`knobDesign` (0 classic, 1 brushed metal), `trackButtonDesign` (0 compact, 1 glass),
`trackFaderDesign` (0 classic, 1 recessed, 2 rounded), `reduceTransparency`,
`selectionTint`, `gridLineWidth`, `gridOpacity`, `playheadWidth`, `playheadTrail`
and `glassMaterial`. The latter uses the existing `vltone.glass` version-1 object
with `dark` and `light` profiles. `appearanceOverrides` applies personal edits
without rewriting a Lua generator. These values are validated before publication
and applied outside drawing; painting continues to read native cached values.

The **Controls and glass** editor tab changes the same document and uses its
Undo/Redo history. Application preview includes these settings. Temporary
material changes do not enter the preference store, including delayed saves and
application shutdown. Applying commits the document; reverting restores the
captured values. An active pointer gesture defers font/style publication along
with geometry so native size hints cannot move the captured control.

Piano Roll exposes separate keys, key labels, rows, bar/beat/subdivision lines,
parameter-lane bars/caps and note faces. Audio editing exposes the waveform,
ruler, grid, regions, fade curve/handle and playhead. Timeline grid, ruler,
selection and cycle graphics have their own IDs. Stroke parts accept
`foreground`, `opacity` and `metrics.lineWidth`; their coordinates stay with the
musical editor. The theme editor disables geometry editing for these objects.
The note layer's `$value` is normalized velocity, from 0 to 1.

The saved-clip browser uses two repeated layouts: `browser.library.row` gives
`browser.library.card` its size; the card recipe places its marker, title,
detail and preview. They share the native list's selection and accessibility
geometry. The Compact example supplies both recipes. The grid, notes, waveform
and automation are independently styled inside the musical preview.

The master VU template (`mixer.master.vu`) exposes its face, scale, title,
unit, two needles, cover, peak bars and clip label. A needle layer receives
the normalized indicated value, so a sprite or rotating image can replace
the factory stroke. Its retained artwork is invalidated on theme publication.

## Persistence, recovery and integration

`.vlttheme` binary version 2 retains the existing streaming container and hashed
resource blocks. Version 1 remains readable. New packages contain the authoring
document, artwork, bundled fonts and the former separate glass/button/knob/fader
preferences. Installed originals stay immutable; active personal changes are
stored separately in application preferences. Appearance and layout publication
are independent choices, and invalid source/resources leave the previous theme.

`--safe-theme` starts from the standard theme without loading custom theme data,
custom interface fonts, background resources, author code or theme layouts.
It does not erase the saved theme. The appearance section also offers Reset.

Register semantic templates with `bindThemeElement`. Repeated tracks share the
same template; widget instances retain their existing track/parameter binding.
`ThemeElementSpec` lists parts, states, properties and existing actions. Object
names and explicit semantic bindings take precedence over declared fallback IDs.
`command.<existing-action-id>` identifies named menu actions without creating
new commands. `ThemeCatalogData.inc` contains compiled descriptors;
`theme-elements.json` records source ownership. That registry is an inventory,
not evidence that every painting branch has passed visual acceptance.

Protect a device root before creating its children with `protectDeviceTheme`.
Use `ThemeContext` both in QWidget paint events and direct `paintScene` calls.
The latter is necessary for retained/GPU rendering. Content invalidation uses
`graphics::updateWidgetContent`, including hidden retained pages.

Images are prepared outside drawing, limited to 16 MiB encoded each, 16 million
pixels each and 128 MiB decoded per theme. Decoded images share a content cache
and Undo snapshots. At most 256 images, 16 fonts, 256 layers per rule, 1,024
items per layout and 16 nesting levels are accepted. The document limit is
2 MiB. Theme history is independent and limited to 100 operations.

Reference concepts: [REAPER WALTER](https://www.reaper.fm/sdk/walter/walter.php)
separates graphics from layout rules;
[Qt QStyle](https://doc.qt.io/qt-6/qstyle.html) provides the native control adapter.

## Verification evidence

The executable `theme_authoring_test` exercises source validation, inheritance,
real widget geometry and hit testing, native-slider dragging, deferred layout,
editor fields and Undo/Redo, repeated atomic external edits, Lua worker failures,
asset snapshots and reset. The full build passes its script-worker path to CTest.
`theme_runtime_ui_test` exercises real transport/track/channel widgets, playback,
project Undo, pointer and keyboard input, wheel routing, readable tempo fields,
and a protected Gain device. `--devices` extends boundary checks to available
built-in editors. `--editors` verifies individual Piano Roll artwork, normalized
velocity bindings, pointer note deletion/project Undo and audio selection. Native control checks have run at 100/125/150/200 percent.
Settings search and the web bookmark/history list also expose a repeated row, entry, icon, title and secondary text. The Compact example puts the two labels on one line above 420 logical pixels and onto two lines in a narrow panel. The settings-shell test checks the actual colored pixels and hit rectangles, accessible path, tooltip, and keyboard navigation.
The saved-clip test changes real row height and note artwork, then selects by
pointer and keyboard. Master VU checks cover independent face artwork and cache
invalidation. Package, appearance-session, editor-window and retained-scene
tests are separate. Windows monitor and installed VST3 checks run explicitly.

See [acceptance evidence and outstanding checks](theme-acceptance.md) and
[before/after measurement data](theme-performance-baseline.json).

### Appearance widgets and winter artwork

A panel with `"activation": "appearance"` may use `browser.footer` or `workspace.status`. The host layout reserves its space. These panels follow the appearance selection while custom window arrangements continue to follow the layout selection. A button may specify an `icon` from the theme assets and an existing `command`. The Christmas example binds its parcel to `file.export` and its snowflake to `view.themeSnow`. The snow command cycles through off, header and application. The choice belongs to the local user. Application snow uses a separate input-transparent surface and leaves insert racks and protected plugin editors clear. Reduced motion freezes visible snow. The optional `workspace.header.glass` style supplies the rounded backing for header controls. `timeline.emptyArtwork` draws image layers below the last track, above the viewport scrollbar. Its `height` and `aspectRatio` metrics control the illustration at different sizes. For art that remains visible over occupied tracks, use `timeline.foregroundArtwork`. A single image layer with `"repeatX": true` repeats at its natural aspect ratio. SVG tiles retain their vectors and are rasterized at the current display scale. Header capsules take their horizontal padding from `workspace.header.glass.metrics.endInset` (factory default 2).

A layered icon button can set `metrics.iconSize` to draw its live command glyph over a theme sprite. The Christmas buttons use two vertical frames for off/on. The `workspace.header.readout` part skins the transport display surround. The optional `headerBackdrop.garland` object accepts `enabled`, one to twelve `colors`, bulb `spacing` (12–160 logical pixels) and `speed` (0–1 cycles per second). Garlands follow the resolved display geometry, use the shared visibility-aware frame clock and stay steady under reduced motion.

The ornament glyph anchor is expressed by normalized `metrics.iconCenterX` and `iconCenterY`. Both default to 0.5. A theme button can use `commandLabel: true` to follow its action's localized caption and accessible name. In Christmas Eve, `view.cycleThemeGarland` cycles independent twinkles, a traveling wave, synchronized breathing and steady light. This preference persists in the local profile. The common `workspace.chrome` artwork skins host panels and editor toolbars. Individual surfaces can override it. `browser.files.emptyArtwork` appears only below the visible file rows. An optional `browser.files.train` layer adds a focusable train to this space. Its vertical sprite frames follow wheel travel. A click or Space starts one journey and reduced motion keeps it at the station.
