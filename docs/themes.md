# Phi themes: agent guide

Read this when the user asks to change Phi's appearance. Themes are JSON data;
ordinary colour, layout, spinner, and tool-presentation changes do not require
an extension or a rebuild. Do not invent theme keys.

## Files and commands

Global configuration is under `$XDG_CONFIG_HOME/phi`, defaulting to
`~/.config/phi`.

- `themes/NAME.json`: shared theme definition, containing presentation settings.
- `theme.json`: default selection for new chats, containing only `{"theme":"NAME"}`.
- `<transcript>.theme.json`: that transcript's independent name-only selection.
- A `themes/` directory beside the executable can also supply definitions;
  user definitions take precedence.

`/theme` lists themes. `/theme NAME` selects one for the current chat.
`/reload` reloads the theme; it does not reload native extensions. Shared
changes also reload automatically. Changing a shared definition affects every
chat selecting it, so preserve the existing file and explain the scope.
`default` uses the built-in look unless a definition with that name exists.

## A small definition

Save this as `themes/workspace.json` in the configuration directory:

```json
{
  "accent": "#91d5a4",
  "markdown.table.rules": "rows"
}
```

Then ask the user to select `/theme workspace`. Do not send slash commands as
ordinary agent-to-agent messages and assume that they execute as commands.

Definitions accept palette indices, hex colours, and supported presentation
settings. Missing keys retain defaults. Unknown keys and malformed settings are
errors. Files are limited to 32 KiB. A definition can use `"extends":"NAME"`
for one level of inheritance; selectors cannot contain presentation overrides.

## Finding the exact schema

There is no general JSON Schema in this distribution. Read `src/ui/theme.inc`
relative to the Phi source root (not the user's current project) for accepted
keys, value types, limits, defaults, and validation. Read `src/core/ui-layout.h`
for layout data structures. Inspect nearby handling of a key before changing it.
Do not infer support merely from another program's theme format.

For native cell programs or custom renderers, read `docs/extensions.md` first.
A theme selecting native code does not itself authorize loading new code.
