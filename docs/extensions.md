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
