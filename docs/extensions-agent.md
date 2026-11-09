# Phi native extensions: agent authoring guide

Use this guide when the user asks for reusable Phi behavior that belongs outside core. Always pair it with the exact `header` returned by `extensions(action:"api")`; that header is the ABI contract for the running Phi binary.

## Before you start

Read this section first. It holds what you cannot guess.

- **One file.** One `.c` file per extension, and the name in its descriptor must equal the file name without `.c`.
- **JSON: use json-c.** Build every JSON text that contains user or model text with json-c (`json_object_new_string`, then `json_object_to_json_string`), never with `snprintf`. Quotes, backslashes and newlines break hand-built JSON. A fixed string with no outside text is fine.
- **Free once.** Free every object you get from `json_tokener_parse` or `json_object_new_*` exactly once with `json_object_put`, and read the strings you need from it *before* freeing it.
- **Return codes.** Registration functions return 0 on success. `api->call` returns 0 on success and 1 on error, with `{"error": ...}` in the output. Check `PHI_API_HAS(api, call)` (or `on`, `exec`, ...) before using a member.
- **Strict C11.** For POSIX functions (`readlink`, `strdup`, `strnlen`, ...), put `#define _POSIX_C_SOURCE 200809L` before every `#include`.
- **Built-in tools:** `read`, `write`, `edit`, `apply_patch`, `git`, `bash`, `bash_start` (a background job), `bash_poll`, `bash_write` (input to a running job), and `bash_kill`. `get_all_tools` also lists extension tools.
- **Guards.**
  - Shell commands reach Phi through `bash` *and* `bash_start`, both with a `command` argument, so a guard must check both.
  - In a `tool_call` payload, `arguments` is already a JSON object. It is a string only when the model sent invalid JSON.
  - A guard fails closed: if it cannot ask (no UI in print mode, or `ui_confirm` fails), it blocks.
  - Match the command's flags, not any letter in it. `strstr(command, "r")` also matches the `r` in `rm` itself. Look at the words after `rm` for `-r`, `-R`, `-rf`, `-fr` or `--recursive`.
  - Do not add checks you do not need. `ui_confirm` already fails without a UI, so you need no separate check for one.
  - Size reply buffers generously (4–8 KiB). A buffer that is too small is an error with `"needed"`, not an empty answer.
- **Dialog answers.** `ui_confirm` answers `{"confirmed": true|false}`. Pass `"default": false` for anything destructive, so that Enter means No. `ui_select` and `ui_input` answer `{"value": ...}`, or `{"cancelled": true}`.
- **Start from a recipe.** Begin with one below, or with a file in `examples/extensions/` (`todo.c`, `plan.c`, `task.c`, `notify.c`, `statusline.c`, `btw.c`), and change as little as you need.

### Recipe: a tool with a structured reply

```c
#include "phi_extension.h"
#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

/* shout: return the text in capitals. */
static int shout(void *userdata, const char *input, char *out, size_t cap)
{
    (void)userdata;
    json_object *args = json_tokener_parse(input ? input : "{}");
    const char *text = json_object_get_string(json_object_object_get(args, "text"));
    json_object *reply = json_object_new_object();
    if (!text) {
        json_object_object_add(reply, "content", json_object_new_string("text is required"));
        json_object_object_add(reply, "is_error", json_object_new_boolean(1));
    } else {
        char upper[4096];
        snprintf(upper, sizeof(upper), "%s", text);
        for (char *c = upper; *c; c++) *c = (char)toupper((unsigned char)*c);
        json_object_object_add(reply, "content", json_object_new_string(upper));
    }
    snprintf(out, cap, "%s", json_object_to_json_string(reply)); /* json-c escapes it */
    json_object_put(reply);
    json_object_put(args); /* after the last use of text */
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, register_tool_ex)) return 1;
    return api->register_tool_ex(api->context,
        "{\"name\":\"shout\",\"description\":\"Return the text in capitals.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
        "\"required\":[\"text\"],\"additionalProperties\":false}}", shout, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "shout", init, NULL
};
```

### Recipe: a guard that asks before a shell command

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const struct phi_api_v1 *phi;

/* confirm_guard: ask before any shell command containing "rm -rf". */
static int on_tool_call(void *userdata, const char *event, const char *payload, char *out, size_t cap)
{
    (void)userdata; (void)event;
    json_object *call = json_tokener_parse(payload);
    const char *name = json_object_get_string(json_object_object_get(call, "name"));
    json_object *arguments = json_object_object_get(call, "arguments");
    const char *command = json_object_get_string(json_object_object_get(arguments, "command"));
    int shell = name && (!strcmp(name, "bash") || !strcmp(name, "bash_start"));
    if (shell && command && strstr(command, "rm -rf")) {
        json_object *request = json_object_new_object();
        json_object_object_add(request, "title", json_object_new_string("Allow this command?"));
        json_object_object_add(request, "message", json_object_new_string(command));
        json_object_object_add(request, "default", json_object_new_boolean(0)); /* Enter means No */
        char answer[512];
        int failed = phi->call(phi->context, "ui_confirm", json_object_to_json_string(request), answer, sizeof(answer));
        json_object_put(request);
        json_object *reply = failed ? NULL : json_tokener_parse(answer);
        int allowed = reply && json_object_get_boolean(json_object_object_get(reply, "confirmed"));
        if (reply) json_object_put(reply);
        /* Fail closed: no answer (no UI, or an error) blocks. */
        if (!allowed) snprintf(out, cap, "{\"block\":true,\"reason\":\"The user did not allow rm -rf.\"}");
    }
    json_object_put(call); /* after the last use of name and command */
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, on) || !PHI_API_HAS(api, call)) return 1;
    phi = api;
    return api->on(api->context, "tool_call", on_tool_call, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "confirm_guard", init, NULL
};
```

### Recipe: a status that updates itself

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <time.h>

static const struct phi_api_v1 *phi;

/* clock: the time in the footer, refreshed every minute by "tick". */
static int on_tick(void *userdata, const char *event, const char *payload, char *out, size_t cap)
{
    (void)userdata; (void)event; (void)payload; (void)out; (void)cap;
    char now[16], text[64], reply[256];
    time_t clock = time(NULL);
    strftime(now, sizeof(now), "%H:%M", localtime(&clock));
    snprintf(text, sizeof(text), "{accent}%s{/}", now); /* markup: theme colours */
    json_object *request = json_object_new_object();
    json_object_object_add(request, "key", json_object_new_string("clock"));
    json_object_object_add(request, "text", json_object_new_string(text));
    phi->call(phi->context, "ui_status", json_object_to_json_string(request), reply, sizeof(reply));
    json_object_put(request);
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, on) || !PHI_API_HAS(api, call)) return 1;
    phi = api;
    char reply[256];
    if (api->call(api->context, "ui_tick", "{\"interval_ms\":60000}", reply, sizeof(reply))) return 1;
    return api->on(api->context, "tick", on_tick, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "clock", init, NULL
};
```

## Workflow

1. Call `extensions` with `action:"api"`.
2. Read `source_directory`, `header`, `guide`, limits, and any scan notice in the result.
3. Choose a valid source basename. The descriptor name must exactly match that basename without `.c`.
4. Write a single C source file into the reported extension source directory when current safety policy permits it. If workspace/strict sandboxing prevents that path, create the source in the project and tell the user where to copy it; do not weaken sandboxing yourself.
5. Call `extensions` with `action:"reload"` and that exact name.
6. The user must explicitly approve new/changed native code. Do not attempt to bypass or automate that consent.
7. If compilation or registration fails, use the returned diagnostics to repair the source and reload again.
8. After loading, exercise the registered tool/command with a small test before relying on it.

Unchanged approved extensions load from Phi's cache on later startups without recompilation. Editing the source creates a new revision and therefore requires a new approval.

## Minimal shape

```c
#include "phi_extension.h"
#include <stdio.h>

static const struct phi_api_v1 *phi;

static int hello(void *userdata, const char *input, char *out, size_t cap)
{
    (void)userdata;
    (void)input;
    snprintf(out, cap, "hello from native Phi");
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    phi = api;
    return api->register_tool(
        api->context,
        "hello_native",
        "Return a small native greeting.",
        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        hello,
        NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1,
    sizeof(struct phi_extension_v1),
    "hello",
    init,
    NULL
};
```

If the file is `hello.c`, the descriptor name must be `hello`.

## Tools

Register tools during `init`. `register_tool_ex` also works later, while the extension is loaded, for tools that appear at runtime (an MCP server that connects later, say); the model sees them from its next request. `api->call(ctx, "unregister_tool", {"name": ...})` removes one, though not while one of your tools is running. Tools added after `init` are not kept across a reload. Tool names must be unique and must not collide with built-ins or another loaded extension. The parameter string is provider-compatible JSON Schema and must describe an object. Phi validates the schema container; the callback must still validate the actual argument JSON it receives.

All registration functions return **0 on success** and nonzero on failure, matching `init`. For a one-registration extension, `return api->register_tool(...);` is correct. For multiple registrations, check each call and return immediately on failure.

A tool callback receives borrowed UTF-8 JSON text and a host-owned output buffer. Write a NUL-terminated UTF-8 result. Return `0` for a normal result and nonzero for an error result. Do not retain input/output pointers after the callback returns.

Use `api->cancelled()` in long synchronous callbacks. Keep native callbacks short when possible; a callback runs on Phi's main thread and can block the UI.

## Structured tools

For anything beyond a text answer, register the tool with `api->register_tool_ex(api->context, definition_json, callback, userdata)` (check `PHI_API_HAS(api, register_tool_ex)`). The definition is JSON: `name`, `description`, `parameters`, and optionally `label` (card heading), `detail_key` (argument shown beside it), `prompt_snippet` (one line added to the system prompt while the tool is active), `prompt_guidelines` (up to 8 rules), `execution_mode` and `structured` (default true).

A tool can opt into `"hide_success": true` in its registration definition. The
terminal UI hides its routine preview and successful cards, while failures and
approval prompts remain visible. This is display only: model context, saved
transcripts, and other clients are unchanged. Restored calls use the current
registration; if the tool is unavailable they remain visible. The default is
false. This option is independent of themes.

A structured tool writes a JSON object result into a 1 MiB buffer:

- `content`: what the model sees (text, or JSON which is serialized).
- `details`: any JSON saved with the session for the extension's own use (for example state to rebuild later from `get_branch`). It is never sent to the model.
- `display`: text shown in the tool card instead of `content`.
- `is_error`: mark the result as an error (returning nonzero also does).
- `terminate`: when every tool in the batch sets it, the run ends without another model request.
- `image_url`: a `data:image/...;base64,` image attached for the model.

While it runs, a tool can stream progress into its card with `api->call(api->context, "tool_update", "{\"text\":\"...\"}", out, cap)`. Keep `content` short; put bulk data in `details` or a file and tell the model where it is.

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const struct phi_api_v1 *phi;

/* count_lines {path}: line count for the model, per-kind details for us. */
static int count_lines(void *userdata, const char *input, char *out, size_t cap)
{
    (void)userdata;
    json_object *args = json_tokener_parse(input);
    const char *path = json_object_get_string(json_object_object_get(args, "path"));
    FILE *file = path ? fopen(path, "r") : NULL;
    json_object *result = json_object_new_object();
    if (!file) {
        json_object_object_add(result, "content", json_object_new_string("Cannot open that file."));
        json_object_object_add(result, "is_error", json_object_new_boolean(1));
    } else {
        char status[128], reply[256];
        long lines = 0, blank = 0;
        char line[4096];
        while (fgets(line, sizeof(line), file)) {
            lines++;
            if (line[strspn(line, " \t\r\n")] == 0) blank++;
            if (lines % 100000 == 0) {
                snprintf(status, sizeof(status), "{\"text\":\"%ld lines so far\\n\"}", lines);
                phi->call(phi->context, "tool_update", status, reply, sizeof(reply));
            }
        }
        fclose(file);
        char text[128];
        snprintf(text, sizeof(text), "%s has %ld lines.", path, lines);
        json_object *details = json_object_new_object();
        json_object_object_add(details, "lines", json_object_new_int64(lines));
        json_object_object_add(details, "blank", json_object_new_int64(blank));
        json_object_object_add(result, "content", json_object_new_string(text));
        json_object_object_add(result, "details", details);
        snprintf(text, sizeof(text), "%ld lines, %ld blank", lines, blank);
        json_object_object_add(result, "display", json_object_new_string(text));
    }
    snprintf(out, cap, "%s", json_object_to_json_string(result));
    json_object_put(result);
    json_object_put(args);
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, register_tool_ex)) return 1;
    phi = api;
    return api->register_tool_ex(api->context,
        "{\"name\":\"count_lines\",\"description\":\"Count lines in a text file.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]},"
        "\"label\":\"Count lines\",\"detail_key\":\"path\","
        "\"prompt_snippet\":\"Use count_lines instead of wc -l.\"}",
        count_lines, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "count_lines", init, NULL
};
```

`register_tool` remains for plain text tools (64 KiB result, no extras).

## Slash commands

Register a command with its leading slash, for example `/hello`. The callback receives the argument text rather than JSON. Set `takes_arguments` accurately so command completion/pickers can describe it correctly.

Use `api->message()` for plain terminal messages. Do not emit terminal escape sequences directly.

## Host services: JSON, model calls and programs

Do not hand-write a JSON parser, a model client or fork/exec code. Every extension is compiled and linked against **json-c**, and the host offers two services:

- `api->complete(api->context, request_json, out, cap)`: a nested model request using the user's existing Phi login. Request `{"messages":[{"role":"user","content":"..."}], "system":"...", "model":"...", "effort":"low"}` (only `messages` is required); result `{"text":"...","model":"...","usage":{...}}`. No tools are offered to the nested request.
- `api->exec(api->context, request_json, out, cap)`: run a program without a shell (`{"argv":["prog","arg"]}`) or a bash command (`{"command":"..."}`), with optional `cwd`, `stdin` and `timeout` (1-600 s, default 30). It follows the user's sandbox settings. Result `{"exit_code":N,"output":"..."}`; output over 256 KiB is an error.

Both take and return JSON object text, return `0` on success and nonzero with `{"error":"..."}` otherwise. If `cap` is too small the error includes `"needed":N`. Check `PHI_API_HAS(api, complete)` / `PHI_API_HAS(api, exec)` in `init` before relying on them. `api->http_request` remains available for direct HTTPS calls.

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const struct phi_api_v1 *phi;

/* Tool: summarize the output of `git log` for a path. */
static int log_summary(void *userdata, const char *input, char *out, size_t cap)
{
    (void)userdata;
    json_object *args = json_tokener_parse(input);
    const char *path = json_object_get_string(json_object_object_get(args, "path"));
    if (!path) { json_object_put(args); snprintf(out, cap, "path is required"); return 1; }

    json_object *run = json_object_new_object(), *argv = json_object_new_array();
    const char *words[] = {"git", "log", "--oneline", "-20", "--", path};
    for (size_t i = 0; i < 6; i++) json_object_array_add(argv, json_object_new_string(words[i]));
    json_object_object_add(run, "argv", argv);
    static char result[300000];
    int failed = phi->exec(phi->context, json_object_to_json_string(run), result, sizeof(result));
    json_object_put(run);
    json_object *ran = json_tokener_parse(result);
    if (failed) { snprintf(out, cap, "git failed: %s", result); json_object_put(ran); json_object_put(args); return 1; }

    json_object *ask = json_object_new_object(), *messages = json_object_new_array(), *message = json_object_new_object();
    json_object_object_add(message, "role", json_object_new_string("user"));
    json_object_object_add(message, "content", json_object_new_string(
        json_object_get_string(json_object_object_get(ran, "output"))));
    json_object_array_add(messages, message);
    json_object_object_add(ask, "messages", messages);
    json_object_object_add(ask, "system", json_object_new_string("Summarize these commits in two sentences."));
    failed = phi->complete(phi->context, json_object_to_json_string(ask), result, sizeof(result));
    json_object *reply = json_tokener_parse(result);
    snprintf(out, cap, "%s", failed ? result : json_object_get_string(json_object_object_get(reply, "text")));
    json_object_put(reply); json_object_put(ask); json_object_put(ran); json_object_put(args);
    return failed;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, complete)) return 1;
    phi = api;
    return api->register_tool(api->context, "log_summary", "Summarize recent git history for a path.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        log_summary, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "log_summary", init, NULL
};
```

Free every json-c object you create (`json_object_put`); objects returned by `json_object_object_get` are borrowed. Services block Phi's main thread while they run, and the user can cancel them.

## Lifecycle events

`api->on(api->context, "event", handler, userdata)` subscribes to one of Phi's lifecycle events; register during `init` (up to 64 handlers) after checking `PHI_API_HAS(api, on)`. The complete list, with each payload and the result that changes behaviour, is the "Event reference" comment in the header returned by `extensions(action:"api")`. It follows Pi's events: sessions (`session_start`, `session_before_compact`, ...), input (`input`, `user_bash`), the agent run (`before_agent_start`, `turn_end`, `message_end`, `agent_before_settle`, ...), provider requests (`context`, `before_provider_request`, `before_provider_headers`, ...), tools (`tool_call`, `tool_result`, ...) settings (`model_select`, `thinking_level_select`) and resources (`resources_discover` adds skill and prompt-template paths; `project_trust` can refuse a project's skills and templates, see docs/resources.md).

A handler receives the event name and its payload as JSON text. To change what happens, write a JSON object result (for example `{"block":true,"reason":"..."}` for `tool_call`) and return `0`; write nothing for no effect. Returning nonzero reports an error, and a failing `tool_call` handler blocks the call. Handlers run in extension load order and each sees the payload as changed by earlier handlers. Events are not delivered while an extension callback is running, so a handler's own `complete`/`exec`/tool work cannot recurse into handlers. Continuation requests (`turn_end`/`agent_before_settle` with `"continue":true`) are capped at 8 per prompt.

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Block destructive shell commands before they run. */
static int guard(void *userdata, const char *event, const char *payload, char *out, size_t cap)
{
    (void)userdata; (void)event;
    json_object *call = json_tokener_parse(payload);
    const char *name = json_object_get_string(json_object_object_get(call, "name"));
    json_object *arguments = json_object_object_get(call, "arguments");
    const char *command = json_object_get_string(json_object_object_get(arguments, "command"));
    int shell = name && (!strcmp(name, "bash") || !strcmp(name, "bash_start"));
    if (shell && command && strstr(command, "rm -rf /"))
        snprintf(out, cap, "{\"block\":true,\"reason\":\"refusing rm -rf /\"}");
    json_object_put(call);
    return 0;
}

/* Give the model the current time with every prompt, as hidden context. */
static int stamp(void *userdata, const char *event, const char *payload, char *out, size_t cap)
{
    (void)userdata; (void)event; (void)payload;
    char now[64];
    time_t clock = time(NULL);
    strftime(now, sizeof(now), "%Y-%m-%d %H:%M %Z", localtime(&clock));
    json_object *result = json_object_new_object();
    char text[128];
    snprintf(text, sizeof(text), "Current time: %s", now);
    json_object_object_add(result, "message", json_object_new_string(text));
    snprintf(out, cap, "%s", json_object_to_json_string(result));
    json_object_put(result);
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, on)) return 1;
    if (api->on(api->context, "tool_call", guard, NULL)) return 1;
    return api->on(api->context, "before_agent_start", stamp, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "guard", init, NULL
};
```

Build results with json-c when they contain text, so quoting and escaping are always correct.

Extensions can also talk to each other: `api->emit(api->context, "name", payload_json)` delivers `{"from":"sender","data":{...}}` to every handler registered for `"ext:name"`.

### Older narrow hooks

`register_hook` (observe-only `turn_start`, `turn_end`, `tool_start`, `tool_end`, `session_load` with a `{"detail":...}` payload), `register_tool_interceptor`, `register_context_provider` and `register_turn_message_provider` keep working for existing extensions. New extensions should use `api->on`: `tool_call` covers interception, `before_agent_start` covers turn messages and prompt context, and the lifecycle events are richer than the old hooks (note that the old hook named `turn_start` fires once per prompt, which `api->on` calls `agent_start`).

The API table can grow by appending fields. `api->size` tells you which tail capabilities the running host supplies. Because you should fetch the API from the running Phi before writing an extension, ordinary agent-authored source normally matches exactly; portable source should check `PHI_API_HAS` before calling newer tail fields.

## Cross-chat bridge and top-level CLI

Use `api->call` for `list_chats {}`, `get_chat {id}`, `get_chat_branch {id}`,
`watch_chats {ids:[...],timeout?}`, `send_chat_message {id,text,origin?}`, and
`create_chat {cwd?,model?,thinking?,title?,text?,origin?}`. These work without a
current session. Branch results are `{chat,entries,last_assistant}`; text is full,
not preview-truncated. Send returns `{accepted,delivery_id,queued}` and creation
returns `{id,delivery?}`. Messages are literal UTF-8 (up to 1 MiB), never local
commands/approval answers. Admission hooks and persistent receiver-side rate
limits cover initial and queued input. Delivery acknowledgement is not execution;
do not retry send/create blindly after timeout or an undersized result buffer.

`register_cli_command(context,name,callback,userdata)` adds `phi NAME ...`.
Check `PHI_API_HAS(api, register_cli_command)`; register only during init. The
callback receives `(userdata,argc,const char *const *argv)`, including NAME as
argv[0], reads untouched stdin, writes stdout/stderr, and returns an exit code.
Approve the module interactively first; CLI execution loads only its approved,
cached owner, with no agent/login/UI. Current-session services are unavailable
there. Built-in names are reserved. Core provides new/send/ls/tail/wait already.
See `docs/agent-bridge.md` and `examples/extensions/agent_bridge.c`.

`external_message_admission` receives `{text,origin}` and may return
`{accept:false,reason}`; errors reject delivery. `external_message_delivered`
reports accepted queue entries. No event or service answers a human approval.

## Session control

`api->call(api->context, "method", request_json, out, cap)` queries and drives the session (check `PHI_API_HAS(api, call)`). The header lists every method; the main ones are:

- `send_user_message {text}`: a visible user message. It starts a run when Phi is idle, or steers the running one at its next boundary.
- `send_message {content, custom_type?, trigger_turn?}`: hidden context the model sees, persisted with the session; `trigger_turn` runs the agent on it.
- `append_entry {custom_type, data}` / `get_custom_entries {custom_type?}`: durable per-session records the model never sees.
- `get_session`, `get_branch`, `get_system_prompt`, `get_commands`: read the current state.
  `get_session.temporary` is true when core `--temp` is active; `file` is empty.
  Extensions must not capture temporary-chat messages or write them to memory.
  This is a process-lifetime policy, including after `new_session`. Core disables
  transcript/draft saving and persistent hosting before extensions initialize.
  Native extensions are trusted, not sandboxed: this flag is not an OS-level
  prohibition on arbitrary files, external logging or provider retention.
- `set_model`, `set_thinking_level`, `set_active_tools {tools|null}`, `set_session_name`, `set_label`.
- `abort`, `shutdown`, `reload` (after the current callback), and, only while no prompt is running, `compact`, `new_session`, `switch_session`, `navigate_tree`, `fork`.

Results use the same convention as `complete`/`exec`. Methods that reshape the session return an error when called mid-run, so call them from commands.

```c
#include "phi_extension.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const struct phi_api_v1 *phi;

/* /review PATH: remember the request, then ask the agent to review the file. */
static int review(void *userdata, const char *arguments, char *out, size_t cap)
{
    (void)userdata;
    if (!*arguments) { snprintf(out, cap, "usage: /review PATH"); return 1; }
    json_object *entry = json_object_new_object(), *data = json_object_new_object();
    json_object_object_add(data, "path", json_object_new_string(arguments));
    json_object_object_add(entry, "custom_type", json_object_new_string("review"));
    json_object_object_add(entry, "data", data);
    char result[512];
    phi->call(phi->context, "append_entry", json_object_to_json_string(entry), result, sizeof(result));
    json_object_put(entry);

    json_object *message = json_object_new_object();
    char text[600];
    snprintf(text, sizeof(text), "Review %s for bugs and summarize the findings.", arguments);
    json_object_object_add(message, "text", json_object_new_string(text));
    int failed = phi->call(phi->context, "send_user_message", json_object_to_json_string(message), result, sizeof(result));
    json_object_put(message);
    snprintf(out, cap, "%s", failed ? result : "Review queued.");
    return failed;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, call)) return 1;
    phi = api;
    return api->register_command(api->context, "/review", "Ask the agent to review a file", 1, review, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "review", init, NULL
};
```

## Asking the user: dialogs, inline feedback, status

Approved native extensions can register dynamic-size native cell programs with
`ui_register_surface {id,path}` (absolute library path; lazy load) and remove them
with `ui_unregister_surface {id}`. The library exports `phi_surface_entry` using
`include/phi_surface.h`. Themes select registered IDs through `composer.surface`,
`transcript.surface`, `footer.surface`, or `board.surface`, with `.rows`/`.percent`
limits. Only the owner's approved host executes native code; separate boards
receive rendered cells asynchronously, never executable paths. Pointer clicks
stay within panels; editor keys and approvals remain Phi-owned. See
[UI surfaces](surfaces.md) and `examples/extensions/surface-program.c`.

Use `ui_status` for routine changing progress/status rather than repeatedly
appending feedback. There are no notification overlays, expiry timers, presentation
modes or `/notices` history. Dialogs and approval safety are unchanged.

The `ui_*` methods of `api->call` are Pi's `ctx.ui`: `ui_confirm`, `ui_select`, `ui_input`, `ui_editor` (multi-line) open a dialog and wait for the answer; `ui_notify` is a compatibility adapter for ordinary inline feedback with a level (`info`, `warning`, `error`), not a toast or notification-history system; `ui_status` sets a named footer slot (up to four per extension; empty text clears). The terminal UI shows dialogs as boxes over the conversation, the console binary asks on the terminal, and RPC clients receive Pi's `extension_ui_request` messages. In print and JSON modes nobody can answer, so dialogs return `{"cancelled":true}`; check `get_session`'s `has_ui` when that matters. Use dialogs from commands and tools, not from event handlers that run on every message. To show information rather than ask: `ui_widget` pins up to eight lines above the prompt (or below it with `"placement":"below_editor"`; `"lines":null` removes it), `ui_header` pins lines at the top of the screen, `ui_footer` replaces the footer line (`"text":null` restores it), `ui_working` changes the line shown while a prompt runs: `message` replaces the word, `detail` the part in brackets (normally time and tokens), `tip` the line under it, and `"visible":false` hides it, and `ui_title` sets the terminal window title. Everything an extension placed is removed when it unloads. For input: `ui_get_editor_text`, `ui_set_editor_text` and `ui_paste` read, replace or add to the prompt being typed; `ui_shortcut` with `{"key":"f5"}` (or a free `ctrl+` letter) makes that key emit the `shortcut` event; a `terminal_input` handler sees every key by name and can return `{"consume":true}`; an `autocomplete` handler receives `{text, word}` when Tab has no file to complete and returns `{"items":[{"value":"@banana"}]}`. `register_flag` (during `init`) claims a command-line option `--name` or `--name=value`, and `get_flag` reads it. To show your own messages, pass `"display":true` to `send_message` or `append_entry`: it appears as a card titled with its `custom_type`, and a `render_message` handler can return `{"text": ...}` to draw it differently. To change how replies look without changing them, handle `render_markdown`: it receives `{text}` (the reply so far, as it streams) and may return `{"text": ...}` to show instead; the saved conversation is untouched. For a model call that appears as it is written, add `"show":"Title"` to the `complete` request, or use `complete_stream` (check `PHI_API_HAS(api, complete_stream)`), whose callback receives each piece of text and can return nonzero to stop. For a model call that runs *beside* everything else, even a running turn, use `complete_async` (check `PHI_API_HAS(api, complete_async)`): it returns an id at once, and `on_text`/`on_done` are called later from Phi's loop, on its thread; with `"show":"Title"` the reply streams into a panel above the prompt and becomes a card after the turn. Its question and answer never enter the model's history (save them with `append_entry`), `complete_cancel` stops one, and unloading stops them all. To let a command run while a turn is going (Enter normally queues it), call `set_command_options` with `{"name":"/btw","during_turn":true}` after registering it. `examples/extensions/btw.c` puts these together. `ui_get_theme`, `ui_list_themes` and `ui_set_theme` (a theme name from `~/.config/phi/themes/NAME.json`, or `{"colors":{"accent":33}}` over the current colours) change the look for the session; `colors` may also hold skin keys such as `"glyph.card"` ([skins.md](skins.md)).

Text for widgets, the header, footer, status slots, the working message, notices and display cards can carry markup in the theme's colours: `{accent}deploying{/} to {b}prod{/}`. The tags are `{accent}`, `{muted}`, `{error}`, `{success}`, `{text}` and `{b}` (bold); `{/}` returns to the normal style, and `{{` is a literal brace. Tags do not nest: each one replaces the last. Unknown tags are shown as written. RPC clients receive the text unchanged.

For something that changes over time, such as a clock, a status line fed by a script, or a progress poll, call `ui_tick` with `{"interval_ms": 1000}` (at least a second; `0` stops) and handle the `tick` event: Phi delivers it to your extension alone, from the terminal UI's idle loop, never inside another callback. Nothing runs unless you ask. To restyle any tool's card, built-in ones included, handle `tool_render`: it receives `{name, label, detail, arguments}` as the card opens and may return `{"title": "{accent}Deploy{/} prod", "lines": ["{muted}3 hosts{/}"]}`. A skin's `tool.title` template covers the common case without code.

```c
#include <phi_extension.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const struct phi_api_v1 *phi;

/* /deploy: pick an environment, confirm, report. */
static int deploy(void *userdata, const char *input, char *out, size_t cap)
{
    (void)userdata; (void)input;
    char reply[1024], done[256];
    if (phi->call(phi->context, "ui_select", "{\"title\":\"Deploy where?\",\"options\":[\"staging\",\"production\"]}",
                  reply, sizeof(reply))) { snprintf(out, cap, "%s", reply); return 1; }
    json_object *answer = json_tokener_parse(reply);
    const char *target = json_object_get_string(json_object_object_get(answer, "value"));
    if (!target) { json_object_put(answer); snprintf(out, cap, "Cancelled."); return 0; }
    json_object *ask = json_object_new_object();
    char question[128];
    snprintf(question, sizeof(question), "Deploy to %s now?", target);
    json_object_object_add(ask, "title", json_object_new_string("Deploy"));
    json_object_object_add(ask, "message", json_object_new_string(question));
    phi->call(phi->context, "ui_confirm", json_object_to_json_string(ask), reply, sizeof(reply));
    json_object_put(ask);
    json_object *confirmed = json_tokener_parse(reply);
    int yes = json_object_get_boolean(json_object_object_get(confirmed, "confirmed"));
    json_object_put(confirmed);
    json_object *notice = json_object_new_object();
    char message[160];
    snprintf(message, sizeof(message), yes ? "Deploying to %s" : "Not deploying to %s", target);
    json_object_object_add(notice, "message", json_object_new_string(message));
    json_object_object_add(notice, "level", json_object_new_string(yes && !strcmp(target, "production") ? "warning" : "info"));
    phi->call(phi->context, "ui_notify", json_object_to_json_string(notice), done, sizeof(done));
    json_object_put(notice);
    if (yes) phi->call(phi->context, "ui_status", "{\"key\":\"deploy\",\"text\":\"deploying\"}", done, sizeof(done));
    json_object_put(answer);
    snprintf(out, cap, "%s", yes ? "Started." : "Cancelled.");
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    if (!PHI_API_HAS(api, call)) return 1;
    phi = api;
    return api->register_command(api->context, "/deploy", "Deploy after asking where", 0, deploy, NULL);
}

PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
    PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "deploy", init, NULL
};
```

## Tool interception

An extension may register one tool interceptor with `register_tool_interceptor`. It runs only for model-requested tools, before ordinary approval and before any tool side effect. It receives the immutable tool name and the current JSON argument object as text. Interceptors run in loaded-extension order, and a later interceptor sees arguments rewritten by an earlier one.

Return `PHI_TOOL_INTERCEPT_ALLOW` to leave the call unchanged. Return `PHI_TOOL_INTERCEPT_DENY` and write a short human-readable reason to block it. Return `PHI_TOOL_INTERCEPT_REPLACE` and write a complete replacement JSON **object** to change the arguments. Tool names cannot be changed. Invalid actions or malformed replacement JSON fail closed.

Argument rewrites happen before Phi's normal approval UI, so any approval is for the actual rewritten arguments. Phi keeps the model's original function call in conversation history and annotates the tool result when an interceptor changed the executed arguments. Do not use interception to evade approval or sandbox policy.

## Persisted hidden turn messages

Use `register_turn_message_provider` for context that belongs to one specific user turn and should remain stable in history, such as a timestamp, branch/build identifier, or one-time environment fact. The callback runs **once when Phi accepts a user message**, receives that message text, and may emit one UTF-8 message. Phi persists it immediately after the visible user message, sends it to the model as an ordinary user-role message, and omits it from the TUI, resume preview and tree.

Register a short `custom_type` discriminator (1-48 letters/numbers/`_`/`-`/`.`). The provider output is bounded to 8 KiB, with a 32 KiB aggregate per accepted user message. Return nonzero or an empty string to emit nothing. The persisted transcript retains the extension name and custom type as Phi-private metadata; provider requests strip that metadata before sending the message.

Use this instead of dynamic context when the value should be generated once. **Do not regenerate timestamps or other historical facts in a dynamic context provider**: that would change the newest request context on every round. Dynamic context remains appropriate for genuinely live state that should be recomputed before every request.

Example:

```c
static int stamp(void *userdata, const char *user_message, char *out, size_t cap)
{
    (void)userdata; (void)user_message;
    snprintf(out, cap, "User message timestamp: 2026-09-23T03:45:00+10:00");
    return 0;
}

static int init(const struct phi_api_v1 *api)
{
    return api->register_turn_message_provider(api->context, "timestamp", stamp, NULL);
}
```

The callback should compute the real timestamp at callback time; the literal above only illustrates the shape.

## Dynamic model context

An extension may register one context provider with `register_context_provider`. Phi calls it before each ordinary model request (including tool-follow-up rounds), after fixed/user/project instructions are assembled. Write optional UTF-8 system-level context into the supplied buffer and return `0`; return nonzero to omit that extension's context for the request. Phi labels each contribution with the extension name and bounds the aggregate.

Context providers should be fast and side-effect free. They can read extension state and in-memory state. A `turn_start` hook runs before the first request, so an extension that needs user-turn-dependent context can observe that event and have its context provider reflect the result. Dynamic extension context is trusted system context because the native extension itself is trusted code.

## State, status and lifecycle

For small durable extension state, use `api->state_get` and `api->state_set`. State is a private string map for this extension, stored by Phi under its XDG state directory rather than in transcripts or sessions. Keys are at most 48 bytes; values are at most 4096 bytes. `state_get` returns `0` when found, `1` when absent, and `-1` on invalid input/error. `state_set(..., NULL)` deletes a key. Writes are atomic. Read state during `init` if needed, but persist new state only from tool, command or hook callbacks; `state_set` deliberately rejects writes during init/shutdown.

Use `api->status_set(api->context, text)` for a short one-line footer contribution. The host prefixes the extension name, combines contributions from loaded extensions and keeps the aggregate bounded. Empty text clears the contribution. This is status, not arbitrary UI injection; do not use terminal escapes or multiline text.

Ordinary private in-memory state may still live in the module while loaded. A successful reload creates a fresh module instance, so only values written through the durable state API survive reload/restart.

If `shutdown` is supplied, release all private state there. It can run after a failed `init`, so cleanup must tolerate partially initialized state. Do not leave threads, signal handlers, background callbacks, or pointers into the unloaded shared object.

## Boundaries

Native extensions are trusted code loaded into Phi's process. They can crash or hang Phi and can access Phi's credentials and network. `/sandbox workspace` and `/sandbox strict` do not isolate a loaded extension. Approval is a consent checkpoint, not a sandbox.

Do not use an extension merely to evade a tool approval, filesystem boundary, network restriction, or other user policy. If the desired feature requires permissions the user has not granted, explain that and wait for the user.

V1 intentionally favors tiny standalone source files. json-c is the only library linked in besides libc. Do not assume local private headers, other linker flags, package downloads, extension-to-extension services, arbitrary UI injection, keybinding APIs, or a scripting runtime exist unless the current API explicitly says so. The footer status API is deliberately narrow and is the only UI contribution in this version. Tool interception and dynamic context affect agent behavior, not arbitrary TUI layout.

If a broken approved extension prevents normal startup, recovery mode is:

```sh
PHI_NO_EXTENSIONS=1 ./phi
```

Use recovery mode to inspect or fix the source, then restart normally.

### Model providers

The ABI supports complete native model backends. Use `register_provider` with a
`struct phi_provider_v1` to advertise model IDs and implement their transport. Use
`wrap_provider` for continuation-based middleware around a provider; `"*"` wraps
every provider and `"codex"` names Phi's built-in backend. Provider auth can be
split into login, refresh-check, refresh and resolve callbacks. Phi persists those
credentials privately and serializes refresh. `http_request` and `open_url` are
available as host services. Normal agent requests and compaction both enter the
same provider dispatcher.

Provider wrappers operate on Phi's **normalized provider request**. They do not
intercept a provider's final private HTTP serialization or headers. A provider owns
that wire protocol. Do not claim a transport-level wrapper exists when only
`wrap_provider` is available.

A native provider can synchronously supervise an external CLI/SDK process and
translate its stream, but V1 has no portable host process API. V1 also has no
host-mediated API for a provider to execute Phi's registered tools while the
provider callback is in progress. Therefore an Agent-SDK-style bridge can let the
external agent own its tools today, but exact Pi-style MCP bridging back through
Phi's normal tool approval/sandbox/TUI path needs an additional host tool-invocation
primitive.

For detailed provider architecture, Pi 0.85.1 mapping, testing requirements, and
the dated Claude subscription-policy notes, read
[`provider-extensions-agent.md`](provider-extensions-agent.md) when working in the
source tree. For installed binaries, use the exact header returned by
`extensions(action:"api")` and treat this embedded guide as the contract.
