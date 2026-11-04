# Phi extensions: agent guide

Use native C extensions for reusable tools, slash commands, hooks, providers,
and native UI behavior. Use theme JSON for ordinary appearance changes.

## Required workflow

1. Call the `extensions` tool with `action: "api"` before implementing anything.
2. Read the returned exact SDK header, contract, authoring guide, and source/cache
   paths. These describe the running executable and take precedence over guesses
   based on another Phi version.
3. Write one `.c` file per extension. Its exported descriptor name must match the
   filename stem. Use json-c to construct JSON containing external text.
4. Request a named reload through `extensions` with `action: "reload"` and
   `name: "NAME"`, or ask the user to run `/reload-extensions NAME`.
5. Wait for explicit human approval. Repair compiler errors, exercise the feature,
   and report what was actually tested.

Never approve native code on the user's behalf or change approval policy to
avoid a prompt. Native extensions execute inside Phi, are not sandboxed, and
can access its credentials or crash the process. Approval is required even
when ordinary tool approval is off.

Sources normally live under `$XDG_CONFIG_HOME/phi/extensions`, defaulting to
`~/.config/phi/extensions`. Use the paths returned by the SDK tool rather than
assuming this default. Approved cached artifacts can load without recompiling.
`PHI_NO_EXTENSIONS=1` disables extensions for recovery. The tool's `disable`
action disables a named extension. `/reload` is a theme command, not an extension
reload command.

## Widgets

Phi provides two distinct widget interfaces. Read the running extension SDK
before implementing either; they are not interchangeable.

### Text widgets

Extensions can use `api->call` with `ui_widget` to place styled lines above or
below the editor. The request contains a `key`, `lines`, and an optional
`placement` of `above_editor` or `below_editor`. Set `lines` to null to remove
that widget. There are at most 12 text widgets, each with up to 8 lines.

Text supports semantic markup such as `{accent}`, `{muted}`, `{error}`,
`{success}`, `{text}`, and `{b}`; `{/}` restores the base style. Related UI
services let extensions contribute headers, footers, and working messages.
Widgets belong to their extension and are removed when it unloads.

### Native interactive widgets

Native header/welcome widgets are shared libraries implementing
[`phi_widget.h`](../include/phi_widget.h). Phi owns terminal output, clipping,
timers, and pointer capture. Callbacks run on the UI event loop: keep them
short and do not introduce terminal escapes, subprocesses, threads, or private
render loops.

Export `phi_widget_entry()`, returning a `struct phi_widget_v1` descriptor with
`abi = PHI_WIDGET_ABI` and `size = sizeof(struct phi_widget_v1)`. Despite the
struct name, the current ABI constant is **2**. Always use the header constant.
The descriptor supplies:

- `width` and `height`: the cell grid, currently limited to 200 by 100 cells.
- `create` and `destroy`: allocate and release per-instance state.
- `tick`: advance animation and report changes.
- `delay_ms`: schedule the next tick; zero means no timer.
- `render`: return a borrowed array of `width * height` cells.
- `input`: handle press, drag, release, click, and control events in local cell
  coordinates. Return the handled, changed, and pointer-capture flags as needed.
- `controls` and `control`: optional control labels and named control handling.

Each cell contains a Unicode glyph, foreground/background colours, and flags.
Glyphs must occupy one terminal cell. Colours use the header's small RGB cube,
or xterm-256 indices when the corresponding flag is set; zero inherits the
background. See the header for the exact encoding.

An approved extension registers a library through `ui_register_header_widget`
with `{id, path}`; `path` must be an existing absolute library path. Themes
select the registered ID, not a library path. Only the owning extension can
unregister it using `ui_unregister_header_widget`, and unloading that extension
removes its registrations. Registration does not change a widget's trust level:
native widgets execute inside Phi and are **not sandboxed**. A theme is not
permission to load arbitrary native code.

A theme can select a registered widget with:

```json
{
  "welcome": "widget",
  "welcome.widget": "my-widget",
  "welcome.life": true,
  "welcome.persist": true
}
```

`welcome.life` is the existing enable switch for native widget playback, not
just Game of Life. `welcome.persist` keeps it visible after the conversation
starts. Check `src/ui/theme.inc` for additional layout and presentation settings.

### Bundled examples

`./build.sh` compiles the bundled modules into `build/widgets/`:

- [`life.c`](../src/widgets/life.c): interactive Game of Life, selected by the
  bundled `gol` theme.
- [`orbit.c`](../src/widgets/orbit.c): animated planet artwork baked into native
  terminal cells. Artwork attribution is in
  [`assets/welcome-planet.LICENSE.md`](../assets/welcome-planet.LICENSE.md).

The IDs `life` and `orbit` are reserved. Inspect `src/ui/cinema.inc` for the
native loader, timing, rendering, and input implementation; `src/ui/widgets.inc`
contains the text-widget implementation. For general native UI surfaces rather
than header/welcome widgets, use the separate `phi_surface.h` contract.

## Documentation and source

Read `docs/extensions-agent.md` for recipes and the detailed authoring contract.
That guide and the public `include/phi_extension.h` are also embedded in Phi and
returned by the SDK tool, even when the source tree is unavailable.

Public UI contracts are in `include/phi_surface.h`, `include/phi_renderer.h`,
`include/phi_widget.h`, and `include/phi_transition.h`. Read the relevant header
before using it, and confirm the running SDK exposes the needed registration
service. API calls return errors when unsupported; do not assume a declaration
alone means the running executable supports a feature.

For implementation details, inspect `src/core/extensions.c`,
`src/core/extension_build.c`, and `src/core/surfaces.c`. Paths are relative to
Phi's source root, not the user's current working directory. This source-only
distribution does not include the examples referenced by some older guide
recipes; use the embedded recipes and exact SDK instead.
