#ifndef PHI_EXTENSION_H
#define PHI_EXTENSION_H
#include <stddef.h>
#include <stdint.h>

/* Native code is trusted, not sandboxed. ABI v1 is synchronous and single-threaded.
   Do not retain call inputs/output buffers or call Phi from a thread or signal.
   Register only during init (tools may also be added later: register_tool_ex).
   Up to 128 tools and 32 commands and hooks per extension.
   The API pointer remains valid through shutdown. */
#define PHI_EXTENSION_ABI_V1 1u
#if defined(_WIN32)
#define PHI_EXTENSION_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define PHI_EXTENSION_EXPORT __attribute__((visibility("default")))
#else
#define PHI_EXTENSION_EXPORT
#endif

/* Write a NUL-terminated UTF-8 result; 0 means success, nonzero means error.
   Tool input is a JSON object string. Command input is its argument text.
   The output capacity is 65,536 bytes, including the terminator. */
typedef int (*phi_extension_call_v1)(
	void *userdata, const char *input, char *output, size_t capacity);
typedef void (*phi_extension_hook_v1)(void *userdata, const char *event, const char *json);

/* A tool interceptor runs before Phi asks for ordinary tool approval or starts
   the tool. It sees the model-requested tool name and current JSON arguments.
   Return ALLOW to leave them unchanged, DENY and write a human-readable reason,
   or REPLACE and write a replacement JSON object into output. Tool names cannot
   be changed. Any other return value or malformed replacement fails closed. */
#define PHI_TOOL_INTERCEPT_ALLOW 0
#define PHI_TOOL_INTERCEPT_DENY 1
#define PHI_TOOL_INTERCEPT_REPLACE 2
typedef int (*phi_extension_tool_interceptor_v1)(void *userdata, const char *tool_name,
	const char *arguments_json, char *output, size_t capacity);

/* Dynamic context is called before each ordinary model request. Write optional
   UTF-8 system-level context into output and return 0. Nonzero means no context
   from this provider for that request. Keep providers fast and side-effect free. */
typedef int (*phi_extension_context_provider_v1)(void *userdata, char *output, size_t capacity);

/* A turn-message provider runs once when Phi accepts a new user message. It may
   append one persisted model-visible/UI-hidden user-role message. This is for
   historical facts such as timestamps: generate them once here rather than in
   dynamic context. Return 0 and write UTF-8 to emit; nonzero or empty output
   emits nothing. custom_type is registered separately and persisted as metadata. */
typedef int (*phi_extension_turn_message_provider_v1)(
	void *userdata, const char *user_message, char *output, size_t capacity);

/* Provider API -----------------------------------------------------------
   Providers receive a Phi-normalized request JSON object and return a JSON
   array of completed output items in result_json. They may stream display
   events while the request is running. Supported event names are:
     text        UTF-8 text delta
     reasoning   UTF-8 reasoning delta (display-only when supported)
     tool        JSON object describing a partial/completed function call
     usage       JSON object {"input_tokens":N,"output_tokens":N}
   Unknown events are ignored by the host so the event vocabulary can grow.
   A provider callback must not retain request/event buffers after returning. */
typedef int (*phi_provider_emit_v1)(void *emit_context, const char *event, const char *data);
typedef int (*phi_provider_stream_v1)(void *userdata, const char *request_json,
	phi_provider_emit_v1 emit, void *emit_context, char *result_json, size_t result_capacity);
typedef int (*phi_provider_auth_call_v1)(
	void *userdata, const char *input_json, char *output_json, size_t capacity);
typedef int (*phi_provider_auth_check_v1)(void *userdata, const char *credential_json);

struct phi_provider_auth_v1 {
	size_t size;
	/* login writes a credential JSON object. refresh receives the current
       credential and writes its replacement. needs_refresh returns nonzero
       when refresh should run. resolve receives the current credential and
       writes provider-private request auth JSON; that value is inserted into
       request.auth for stream(). All fields are optional. */
	phi_provider_auth_call_v1 login;
	phi_provider_auth_check_v1 needs_refresh;
	phi_provider_auth_call_v1 refresh;
	phi_provider_auth_call_v1 resolve;
};

struct phi_provider_v1 {
	size_t size;
	const char *id; /* stable provider id, extension-name syntax */
	const char *name; /* human-readable name, 1-128 UTF-8 bytes */
	/* JSON array of model objects. Every object requires id; optional fields:
       name, context_window, capabilities. Model ids must be globally unique. */
	const char *models_json;
	struct phi_provider_auth_v1 auth;
	phi_provider_stream_v1 stream;
	void *userdata;
};

typedef int (*phi_provider_next_v1)(void *next_context, const char *request_json,
	phi_provider_emit_v1 emit, void *emit_context, char *result_json, size_t result_capacity);
typedef int (*phi_provider_wrapper_v1)(void *userdata, const char *provider_id,
	const char *request_json, phi_provider_next_v1 next, void *next_context,
	phi_provider_emit_v1 emit, void *emit_context, char *result_json, size_t result_capacity);

typedef int (*phi_http_chunk_v1)(void *userdata, const void *data, size_t size);

/* Lifecycle event handler. payload_json is the event's JSON object. Write an
   optional JSON object into result to change what happens (leave it empty
   for no effect) and return 0; nonzero reports a handler error. Handlers run
   in extension load order and each sees the payload as changed by earlier
   handlers. Events are not delivered while an extension callback is already
   running (so a handler's own tool or model calls cannot recurse).

   Event reference: payload -> optional result
   Session
     session_start          {reason:"startup"|"new"|"resume"|"fork"|"clone", session_file}
     session_shutdown       {reason:"exit"|"new"|"resume"|"fork"|"clone", session_file}
     session_info_changed   {name}
     session_before_switch  {reason:"new"|"resume", target_file}  -> {cancel:true}
     session_before_fork    {kind:"fork"|"clone", entry_id}       -> {cancel:true}
     session_before_tree    {target_id, current_id}               -> {cancel:true}
     session_tree           {previous_id, current_id}
     session_before_compact {reason:"auto"|"manual", focus, tokens_before,
                             messages:[items to be summarized], kept}
                            -> {cancel:true} | {summary:"replacement summary"}
     session_compact        {summary, from_extension, tokens_before}
     session_compact_failed {error}
   Input
     input                  {text, source:"interactive"}
                            -> {action:"transform", text} | {action:"handled"}
     user_bash              {command, send}  -> {handled:true, output, exit_code}
   Agent run (one per submitted prompt) and turns (one per model request)
     before_agent_start     {prompt, message_type:"user"|"custom", message?}
                            -> {message:"hidden context for this prompt",
                             system_prompt:"replace for this run",
                             system_prompt_append:"add for this run"}
                            message_type "custom": a run send_message trigger_turn
                            started; message is {content, custom_type, extension}
     agent_start            {prompt}
     turn_start             {turn}
     message_start          {role:"assistant"}
     message_update         {delta}
     message_end            {message} -> {text:"replacement assistant text"}
     turn_end               {turn, tool_calls, status} -> {continue:true, message}
     agent_before_settle    {status} -> {continue:true, message}
     agent_end              {status:"completed"|"cancelled"|"failed"}
     agent_settled          {status}
     ("continue" requests one more model request; message is added as hidden
      user context first. At most 8 continuations per prompt.)
   Provider
     context                {messages:[request input items]} -> {messages}
     context_with_system    {messages:[{role:"system",content}, ...]} -> {messages}
     before_provider_request {purpose:"agent"|"compaction"|"extension", request}
                            -> {request:replacement body}
     before_provider_headers {headers:{name:value}} -> {headers:{name:value to set}}
     after_provider_response {purpose, status, ok}
     model_route            {model, reason:"user"|"continuation"|"direct", effort,
                            previous?:{model, effort}, state?}
                            -> {model:"real model id", effort?, state?}
                            before each request while a virtual model this
                            extension registered is selected, to it alone.
                            "direct" is compaction (no previous or state). Pick
                            previous for continuations to keep the prompt cache.
                            state is kept for the conversation in memory only
                            (use set_state to keep it across restarts); no
                            answer sends the virtual id, and the request fails.
     provider_stream_event  {provider, api, model, data} each parsed stream event
                            before Phi normalizes it; read-only, not saved. Slow
                            handlers slow the stream.
   Tools
     tool_call              {call_id, name, arguments, parent_call_id?}
                            -> {block:true, reason} | {arguments:{...}}
                            (a failing tool_call handler blocks the call)
     tool_execution_start   {call_id, name, arguments, parent_call_id?}
     tool_execution_update  {call_id, name, text, parent_call_id?}
     tool_execution_end     {call_id, name, result, is_error, duration_ms, parent_call_id?}
     tool_result            {call_id, name, arguments, result, is_error, parent_call_id?}
                            -> {result:{replacement result object}}
                            parent_call_id: a call another tool made with
                            execute_tool; its call_id is "<parent>/<n>"
   Settings and prompts
     model_select           {model, previous, source}
     thinking_level_select  {level, previous, source}
     external_message_admission {text, origin:{via,actor,pid,uid,delivery_id}}
                            -> {accept:false, reason} blocks external delivery;
                               errors fail closed. No rewriting or approval API.
     external_message_delivered {text, origin} (accepted into the external queue)
     ui_prompt_start        {kind:"tool_approval"|"native_approval", name, question, call?}
     ui_prompt_end          {kind, name, approved}
   Input (phase 5)
     shortcut               {key, extension} (a key registered with ui_shortcut)
     terminal_input         {key: "a" | "ctrl+t" | "up" | "enter" | ...} -> {consume:true}
     autocomplete           {text, word} on Tab -> {items:[{value, label?} | "value"]}
     render_message         {extension, custom_type, content} -> {text:"shown instead"}
     render_markdown        {text} (a reply as it streams) -> {text:"shown instead"}
                            display only: the saved reply and the model's view are unchanged
     tool_render            {name, label, detail, arguments, restored?} as a tool card
                            opens, for any tool, also cards rebuilt on resume
                            (restored:true; the tool may not be registered)
                            -> {title?: "heading (markup)", lines?: ["up to 8 (markup)"]}
                            lines replace the argument list of extension tools
     tick                   {interval_ms} every interval asked for with ui_tick, from
                            the terminal UI's idle loop (never inside a callback)
   Resources (at startup and on /reload)
     resources_discover     {cwd, reason:"startup"|"reload"}
                            -> {skill_paths:[...], prompt_paths:[...]} (added)
     project_trust          {cwd, skill_paths, prompt_paths} (project resources found)
                            -> {trusted:false} skips the project's skills and prompts
   Extension bus: "ext:<name>" receives {from, data} sent with api->emit. */
/* Streamed text for complete_stream; return nonzero to stop. */
typedef int (*phi_text_v1)(void *userdata, const char *text);
/* complete_async finished: result_json is as complete's result, or
   {"error":"..."} ({"error":"Stopped"} after on_text returned nonzero). */
typedef void (*phi_done_v1)(void *userdata, const char *result_json);
typedef int (*phi_event_handler_v1)(
	void *userdata, const char *event, const char *payload_json, char *result, size_t capacity);

struct phi_api_v1 {
	uint32_t abi_version;
	size_t size;
	void *context; /* Opaque registration owner; pass back unchanged. */
	/* Registration functions return 0 on success and nonzero on failure. */
	int (*register_tool)(void *context, const char *name, const char *description,
		const char *parameters_json, phi_extension_call_v1 call, void *userdata);
	/* Slash is included in name. takes_arguments is 0 or 1. */
	int (*register_command)(void *context, const char *name, const char *description,
		int takes_arguments, phi_extension_call_v1 call, void *userdata);
	/* Events: turn_start, turn_end, tool_start, tool_end, session_load.
       Hooks observe events; they cannot replace prompts, results or policy. */
	int (*register_hook)(
		void *context, const char *event, phi_extension_hook_v1 hook, void *userdata);
	void (*message)(const char *text); /* Plain text, control bytes escaped. */
	int (*cwd)(char *buffer, size_t capacity); /* 0 on success; -1 on failure. */
	int (*cancelled)(void); /* Poll in long-running callbacks. */
	/* Small durable string state, private to this extension. state_get returns
       0 when found, 1 when absent, -1 on invalid input/error. state_set writes
       atomically; value == NULL deletes the key. Keys are <=48 bytes, values
       <=4096 bytes. state_set is unavailable during init/shutdown. */
	int (*state_get)(void *context, const char *key, char *buffer, size_t capacity);
	int (*state_set)(void *context, const char *key, const char *value);
	/* Publish a short one-line footer contribution. Empty text clears it.
       The host prefixes the extension name and may truncate aggregate status. */
	int (*status_set)(void *context, const char *text);
	/* Added tail fields keep the original V1 function-table prefix stable.
       At most one interceptor and one dynamic-context provider per extension. */
	int (*register_tool_interceptor)(
		void *context, phi_extension_tool_interceptor_v1 interceptor, void *userdata);
	int (*register_context_provider)(
		void *context, phi_extension_context_provider_v1 provider, void *userdata);
	/* Persisted hidden message provider. custom_type is an extension-local
       discriminator (letters/numbers/_/.-, <=48 bytes). At most one per extension. */
	int (*register_turn_message_provider)(void *context, const char *custom_type,
		phi_extension_turn_message_provider_v1 provider, void *userdata);

	/* Low-level model backends. A provider owns its model transport. Wrappers
       compose around providers and may target one provider id or "*". */
	int (*register_provider)(void *context, const struct phi_provider_v1 *provider);
	int (*wrap_provider)(
		void *context, const char *provider_id, phi_provider_wrapper_v1 wrapper, void *userdata);

	/* Provider-scoped durable credentials. provider_id must name a provider
       registered by this extension. Values are JSON objects, stored privately
       with atomic replacement. These helpers are also available to provider
       code that elects to manage authentication itself. */
	int (*credential_get)(void *context, const char *provider_id, char *buffer, size_t capacity);
	int (*credential_set)(void *context, const char *provider_id, const char *credential_json);
	int (*credential_delete)(void *context, const char *provider_id);

	/* Host HTTP transport. headers_json is a JSON object of string values.
       body may be NULL. chunk may be NULL to discard the response body.
       Returns 0 for a completed HTTP exchange (including non-2xx status), -1
       for local/transport failure or cancellation. */
	int (*http_request)(void *context, const char *method, const char *url,
		const char *headers_json, const char *body, phi_http_chunk_v1 chunk, void *userdata,
		long *http_status, char *error, size_t error_capacity);
	int (*open_url)(void *context, const char *url);

	/* Host services. Requests and results are JSON object strings; parse
       and build them with json-c (<json-c/json.h>), which every extension
       is compiled and linked against. Each call returns 0 and writes a JSON
       result object into output, or returns nonzero and writes
       {"error":"..."}. When capacity is too small the result is
       {"error":"...","needed":N}; retry with at least N bytes. These entries
       were added after the first v1 hosts: check PHI_API_HAS(api, complete)
       before calling them. */

	/* Nested model request using Phi's own login and provider routing.
       Request: {"messages":[{"role":"user"|"assistant","content":"..."}],
                 "system":"optional instructions", "model":"optional id",
                 "effort":"off..ultra, as /effort (optional; the model's nearest level is used)"}
       Result:  {"text":"reply","model":"...","usage":{"input_tokens":N,
                 "output_tokens":N}}. Blocks until done; cancellable by the user.
       "show":"Title" draws the reply live in a card as it is written; for the
       text itself as it arrives, use complete_stream (below).
       No tools are offered to the nested request. */
	int (*complete)(void *context, const char *request_json, char *output, size_t capacity);

	/* Run a program without a shell, under Phi's sandbox settings.
       Request: {"argv":["prog","arg",...]} or {"command":"bash text"},
                 optional "cwd", "stdin" (text) and "timeout" (1-600 s, default 30).
       Result:  {"exit_code":N,"output":"combined stdout+stderr"}. Output is
       limited to 256 KiB; larger output fails with an error. */
	int (*exec)(void *context, const char *request_json, char *output, size_t capacity);

	/* Subscribe to a lifecycle event or "ext:<name>" (see the reference above).
       Register during init; up to 64 handlers per extension. Check
       PHI_API_HAS(api, on) first. */
	int (*on)(void *context, const char *event, phi_event_handler_v1 handler, void *userdata);
	/* Send {from, data} to every "ext:<name>" handler; payload_json is an
       object or NULL. Returns 0, or nonzero if a handler failed. */
	int (*emit)(void *context, const char *name, const char *payload_json);

	/* Session control and queries: call a host method by name with a JSON
       object request; same result convention as complete/exec. Check
       PHI_API_HAS(api, call) first. Methods marked (idle) replace or reshape
       the session and are only allowed while no prompt is running (from a
       command, or an event outside a run); they are rejected otherwise.
       Messages
         send_user_message  {text}            -> {queued:true}  visible user message:
                            starts a run when idle, else steers the running one
         send_message       {content, custom_type?, trigger_turn?, display?}
                            -> {}  hidden context the model sees (persisted);
                            trigger_turn runs the agent on it
         append_entry       {custom_type, data, display?}  -> {}  persisted, never sent to the model
                            display:true (either call) shows it as a card titled custom_type;
                            a render_message handler may supply the text shown
         get_custom_entries {custom_type?}    -> {entries:[{extension,custom_type,data,time}]}
       Session
         get_session        {} -> {file, name, head, idle, mode, has_ui, cwd, model,
                                   effort, tokens_estimate, context_window}
                            mode: interactive|print|json|rpc; has_ui: dialogs
                            can be answered (interactive and rpc)
         get_branch         {} -> {items:[conversation items on the active branch],
                            streaming?:{text, tool?}}  streaming: the reply still
                            arriving (text so far, a tool call being written)
         get_system_prompt  {} -> {text}
         set_session_name   {name} -> {}
         set_label          {entry_id, label, bookmarked?} -> {}
         get_commands       {} -> {commands:[{name, description, takes_arguments}]}
         has_pending_messages {} -> {pending, count}  queued steering/follow-ups
         set_command_options {name, during_turn} -> {}  during_turn:true runs the
                            command at once while a turn runs (Enter otherwise
                            queues it); for /btw-style side commands. Its output
                            shows in a side panel, then as a card after the turn
       Model and tools
         set_model          {model}            -> {}  (as /model)
         set_thinking_level {level}            -> {}  off, minimal, low, medium, high, xhigh, max or ultra (as /effort)
         get_all_tools      {} -> {tools:[names]}
                            {details:true} -> {tools:[{name, description, parameters,
                            annotations, active, source:"builtin"|"extension", label?}]}
         get_active_tools   {} -> {tools:[names]}
         set_active_tools   {tools:[names]}    -> {}  others are hidden from the model
         get_callable_tools {} -> {tools:[details as above]}  what execute_tool can run
         execute_tool       {name, arguments?} -> {result, is_error, details?}
                            run another tool from inside this extension's running
                            tool (Pi's ctx.executeTool). Interceptors, tool_call and
                            tool_result handlers and execution events apply (with
                            parent_call_id); no approval prompt and no transcript
                            entry: only the calling tool sees the result, so report
                            it yourself. Not the extensions tool; 8 levels deep and
                            256 calls per tool call at most.
         get_settings       {} -> {model, effort, approval, sandbox}  in effect now
         register_virtual_model {id, name?} -> {}  a model /model can select
                            whose requests this extension routes (model_route);
                            ids unique across extensions, 8 per extension
         unregister_virtual_model {id} -> {}
       Control
         abort              {} -> {}  cancel the running prompt
         shutdown           {} -> {}  exit after current work
         compact            {focus?} -> {}  (idle)
         new_session        {} -> {}  (idle)
         switch_session     {file} -> {}  (idle)
         navigate_tree      {entry_id} -> {}  (idle)
         fork               {entry_id} -> {}  (idle) new session from that entry
         reload             {} -> {}  reload extensions after this callback returns
       UI (Pi's ctx.ui; answered by the terminal UI, a console prompt, or the RPC
       client; in print/JSON modes dialogs return {cancelled:true})
         ui_confirm         {title, message, timeout_ms?, default?} -> {confirmed, cancelled?}
                            default: false starts on No (use it for destructive actions)
         ui_select          {title, options:[text], timeout_ms?} -> {value, index} | {value:null, cancelled}
         ui_input           {title, placeholder?, value?, timeout_ms?} -> {value} | {value:null, cancelled}
         ui_editor          {title, value?, timeout_ms?} -> {value} (multi-line) | {value:null, cancelled}
         ui_notify          {message, level?: info|warning|error} -> {}
                            compatibility inline feedback; no toast overlay or history
                            use ui_status for routine changing footer status
         ui_status          {key, text} -> {}  a footer slot per key (4 per extension); "" clears
         ui_widget          {key, lines:[text] | null, placement?: above_editor|below_editor} -> {}
                            up to 8 lines by the prompt; null removes (12 widgets in all)
         ui_register_surface {id, path} -> {}  approved native caller registers a
           lazy phi_surface.h program for composer/transcript/footer/board panels;
           boards receive rendered cells from its host, never executable paths
         ui_unregister_surface {id} -> {}  only its owner may remove it
         ui_register_header_widget {id, path} -> {}  approved native caller registers
                            an absolute library path; theme selects ID, loading is lazy
         ui_unregister_header_widget {id} -> {}  only the owning extension may remove it
         ui_register_header_transition {id, path} -> {}  approved native caller registers
           a lazy-loaded phi_transition.h provider; themes select its ID, not its path
         ui_unregister_header_transition {id} -> {}  only its owner may remove it
         ui_composer        {label?, hint?} -> {}  bounded templates; nulls restore theme;
                            yields to prompts/approval and clears when owner unloads
         ui_header          {lines:[text] | null} -> {}  pinned at the top (4 lines)
         ui_footer          {text | null} -> {}  replaces the footer; null restores it
         ui_working         {message?, detail?, tip?, visible?} -> {}  the line while a prompt
                            runs: message replaces the word, detail the "(12s · ...)" part,
                            tip the line under it; omitted parts return to the skin's
         ui_title           {title} -> {}  the terminal window title
         ui_get_editor_text {} -> {text}      the prompt being typed
         ui_set_editor_text {text} -> {}      replace it
         ui_paste           {text} -> {}      insert at the cursor
         ui_get_tools_expanded / ui_set_tools_expanded {expanded} -> {expanded} / {}
         ui_get_theme       {} -> {name, colors:{setting: 0-255 | "default"}}
         ui_list_themes     {} -> {themes:[names]}  from ~/.config/phi/themes/NAME.json
         ui_set_theme       {name?, colors?} -> {}  a named theme and/or settings over the
                            current one, for this session (/reload restores theme.json);
                            colors may hold skin keys too ("glyph.card", docs/skins.md)
         ui_tick            {interval_ms} -> {}  a "tick" event for this extension every
                            1000-3600000 ms; 0 stops (terminal UI only)
         ui_shortcut        {key} -> {}  f1-f12 or ctrl+<letter> (not c d f g h i j m o
                            r v y z); pressing it emits the "shortcut" event
       Flags (during init): register_flag {name} claims --name / --name=value;
         get_flag {name} -> {value: true | "text" | null}. Unclaimed options are
         an error (a warning in the interactive UI, so an extension can be approved).
       Widgets, header and footer belong to the extension and go when it unloads.
       Markup: text for widgets, header, footer, status, working, notices and
         display cards may use {accent} {muted} {error} {success} {text} {b};
         {/} returns to the normal style and {{ is a literal brace. Unknown
         tags show as written. Other clients (RPC) receive the text as is.
       RPC clients get setWidget and setTitle; header, footer and working are
       terminal-only (as in Pi).
       Dialogs block until answered; they are for commands and tools, not for
       event handlers on hot paths. */
	int (*call)(
		void *context, const char *method, const char *request_json, char *output, size_t capacity);

	/* Register a tool from a Pi-style JSON definition, during init or at any
       later time while loaded (the model sees it from its next request; an
       active-tool list set with set_active_tools must name it). Remove one
       with api->call(ctx, "unregister_tool", {"name"}), except while one of
       this extension's tools is running. Tools added later are not kept
       across a reload: register them again from init or when needed.
         {"name", "description", "parameters": {JSON Schema object},
          "label": "card heading", "detail_key": "argument shown beside it",
          "prompt_snippet": "one line added to the system prompt",
          "prompt_guidelines": ["up to 8 usage rules"],
          "execution_mode": "sequential"|"parallel", "structured": true,
          "annotations": {"readOnlyHint", "destructiveHint", "idempotentHint",
                          "openWorldHint": booleans, as MCP tool annotations}}
       Annotations are hints, not verified; a permission extension can use them
       (get_all_tools {details:true}) to decide which calls to confirm. Missing
       hints mean: not read-only, may be destructive, may reach an open world.
       Structured tools (the default) write a JSON object result, and get a
       1 MiB output buffer:
         {"content": "text (or JSON) the model sees",
          "details": any JSON kept with the session but never sent to the model,
          "display": "text shown in the tool card instead of content",
          "is_error": false, "terminate": false,
          "image_url": "data:image/png;base64,..." (attached for the model)}
       Output that is not a JSON object, or an object without "content", is
       given to the model as it is; returning
       nonzero marks an error. "terminate": true in every result of a batch
       ends the run without another model request. While the tool runs,
       api->call(ctx, "tool_update", {"text": "..."}) streams progress into
       its card. Phi runs tools one at a time, so "parallel" is accepted but
       executed sequentially. Check PHI_API_HAS(api, register_tool_ex). */
	int (*register_tool_ex)(
		void *context, const char *definition_json, phi_extension_call_v1 call, void *userdata);

	/* complete, streaming: on_text receives the reply as it is written
       (return nonzero to stop early); the result is as for complete. With
       "show":"Title" in the request (here or in complete), Phi also draws the
       reply live in a card. Check PHI_API_HAS(api, complete_stream). */
	int (*complete_stream)(void *context, const char *request_json, phi_text_v1 on_text,
		void *userdata, char *output, size_t capacity);

	/* complete, concurrently: returns at once, and the request runs beside
       whatever else is happening (even a running turn), on its own
       connection. on_text (optional) gets the reply as it is written, and
       on_done gets the result once. Both run on Phi's thread, between other
       work, never inside complete_async itself. Returns the request's id
       (> 0), or 0 with {"error"} in output. "show":"Title" in the request
       streams the reply into a panel above the prompt; when it finishes it
       becomes a card in the conversation (after the turn, if one is
       running). Neither the question nor the answer enters the model's
       history; save them with append_entry if they should be kept. Requests
       end when the extension unloads (no callback then). Models served by
       an extension provider, and the console and print/JSON/RPC modes, run
       the request at once instead; on_done still comes later in the terminal
       UI, but in the other modes it is called before complete_async returns.
       Check PHI_API_HAS(api, complete_async). */
	int (*complete_async)(void *context, const char *request_json, phi_text_v1 on_text,
		phi_done_v1 on_done, void *userdata, char *output, size_t capacity);
	/* Stop a complete_async request: no further callbacks. 0 if it was
       running, nonzero for an unknown or finished id. */
	int (*complete_cancel)(void *context, int id);

	/* Cross-chat methods through call (also available without a current session):
         list_chats {} -> [status]
         get_chat {id} -> status
         get_chat_branch {id} -> {chat, entries, last_assistant}
         watch_chats {ids:[id,...], timeout?:0..604800} -> first non-working branch
         send_chat_message {id,text,origin?:object} -> {accepted,delivery_id,queued}
         create_chat {cwd?,model?,thinking?,title?,text?,origin?:object} -> {id,delivery?}
       Message text: valid UTF-8, no NUL, <=1 MiB. origin is caller-supplied
       metadata (<=4096 encoded bytes), separate from receiver-recorded pid/uid.
       Delivery is literal user input, never a slash command or approval answer.
       Receiver admission/rate limits apply to initial and mid-turn messages.
       Acknowledgement means queued, not executed; do not blindly retry mutating
       calls after a timeout or undersized output buffer. The queue is in memory.
       Status states: working, idle, needs_input (approval object), ended.
       watch_chats is synchronous and cancellable; CLI waits use inotify/pidfds.
       See docs/agent-bridge.md for the full contract. */

	/* Top-level `phi NAME ...`, not a slash command. Register during init.
       Only already-approved modules load for CLI dispatch; no agent/session is
       started. argc/argv include NAME as argv[0]. stdin is untouched; the
       callback may use stdio and returns a shell exit code (0..255). Startup
       and shutdown output is redirected to stderr. Built-in names are reserved.
       Check PHI_API_HAS(api, register_cli_command). */
	int (*register_cli_command)(void *context, const char *name,
		int (*callback)(void *userdata, int argc, const char *const *argv), void *userdata);

	/* http_request with response headers. request_json is {method, url,
       headers?:{name:value}, body?:"text", timeout_ms?}; the url is https,
       or http on localhost, 127.0.0.1 or [::1] (local servers). The body
       goes to chunk (may be NULL). response_json receives {status,
       headers:{lowercased name: value}} or {error}. Returns 0 for a
       completed exchange (any status), -1 for failure or cancellation.
       Check PHI_API_HAS(api, http_request_ex). */
	int (*http_request_ex)(void *context, const char *request_json, phi_http_chunk_v1 chunk,
		void *userdata, char *response_json, size_t capacity);
};

/* True when the host's API table includes member (for fields added later). */
#define PHI_API_HAS(api, member) \
	((api)->size >= offsetof(struct phi_api_v1, member) + sizeof((api)->member))

struct phi_extension_v1 {
	uint32_t abi_version;
	size_t size;
	const char *name; /* Must equal the source basename, without .c. */
	int (*init)(const struct phi_api_v1 *api); /* 0 on success. */
	/* Called after any attempted init, even a failed one. Release all private
       state; do not leave threads, signal handlers or callbacks into this DSO. */
	void (*shutdown)(void);
};

/* Export a descriptor variable named phi_extension, for example:
   PHI_EXTENSION_EXPORT const struct phi_extension_v1 phi_extension = {
       PHI_EXTENSION_ABI_V1, sizeof(struct phi_extension_v1), "hello", init, NULL
   }; */
#endif
