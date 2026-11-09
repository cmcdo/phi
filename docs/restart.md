# Restart a chat on the latest binary

Rebuild Phi with `./build.sh`, then enter this in the chat terminal:

```text
/restart
```

Phi saves the active conversation and branch selection, cleanly unloads its
extensions and terminal UI, and replaces its process with the executable now
installed at its original pathname. It resumes the same transcript without
replaying the original command-line prompts or running conversation tools.
The command restarts; it does not run a build for you.

## What survives

- Saved conversation and active branch selection.
- Working directory and current model/thinking effort.
- Approval and sandbox policy, including command-line overrides.
- Extension command-line flags and extension state persisted to disk.
- For hosted chats: the same chat ID, agent PID, host socket, and attached PTY.
  The host itself is not upgraded by this operation.

The replacement executable is opened before shutdown, so an atomic rebuild
cannot make the handoff accidentally execute the old inode. This implementation
uses Linux `/proc/self/exe` and `fexecve`, and expects a native ELF executable.

## Safeguards and limitations

- Enter `/restart` locally in an interactive terminal chat. It is not a board,
  print/JSON/RPC, extension, or bridge-control operation.
- Restart is idle-only. A command submitted during a turn waits in the local
  command queue rather than becoming model input.
- Running tools, extension activity/side requests, queued messages, drafts, and
  background jobs with active collectors or unread output block restart.
  Finish/cancel jobs and drain their output with `bash_poll` first. Poll completed
  jobs at least once to acknowledge their exit status, even if they emitted no
  output.
- Drafts are not transferred. If another draft or queued message is pending,
  restart refuses rather than silently discarding it.
- A conversation must already be saved. Empty new chats and `--no-session`
  chats cannot restart.
- Scroll position, editor undo state, temporary panels, and extension-only
  in-memory state reset. Extensions reload normally and must persist any state
  they need across restarts. Native-code approval requirements are unchanged.
- If saving or opening the replacement fails, Phi refuses before shutting down
  and the current chat stays open. If the final OS exec fails despite preflight
  checks (for example, an incompatible ELF or missing dynamic loader), Phi
  exits with an error and prints the saved session path for manual resumption.
  A replacement that itself crashes or rejects startup options likewise needs
  manual recovery with a working binary and `--session PATH`.

You need to start a binary containing `/restart` once before it can perform
future upgrades. An already-running older version cannot acquire this command
just because its file was rebuilt.

## Verification

After building:

```sh
bash tests/test-restart.sh
```

The tests use temporary configuration/state directories and local PTYs, with no
model requests or real credentials. They check atomic binary replacement in
hosted and unhosted chats, session/PID/socket continuity, policy preservation,
save/open refusal paths, and background-job/unread-output guards.
