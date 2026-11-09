# Temporary chats

Start an unsaved foreground chat with the core flag:

```sh
phi --temp
```

No extension or launcher is required. Phi decides this policy before opening an
agent, terminal UI, or extension. For the lifetime of that process:

- No conversation transcript is created or reopened.
- Draft recovery is neither read nor written, even if `PHI_DRAFT_RECOVERY=1`.
- Persistent `phi-host` hosting is skipped, so no hosted scrollback is recorded.
- `/new` remains temporary; it does not re-enable saving.
- Resume/continue/session-file startup and `--bg` are rejected. Switching to a
  saved session at runtime is rejected too. Fork/clone/restart require a saved
  session and are unavailable.
- `/session` and `/transcript` report that saving is disabled.

The SDK exposes `get_session.temporary`; RPC `get_state` exposes `temporary` too.
A cooperating memory extension may support `phi --temp --mem=NAME` to read an
explicitly selected collection without capturing this chat or modifying notes.
The installed memory extension must support the metadata for that integration.
With no memory selection, memory remains off.

`--no-session` retains its narrower meaning: it only disables transcript saving.
It does not imply `--temp` or change draft/hosting policy. `--t` is not an alias.

Temporary mode is **not** a filesystem sandbox, a provider privacy promise, or a
secure-erasure mechanism. Explicit file-writing tools, `/export`, your terminal's
own logging, and trusted native extensions can still save data. Authentication,
configuration and compiler caches may still be updated. It does not delete old
transcripts, drafts or memories. Use normal approval/sandbox controls as needed.
