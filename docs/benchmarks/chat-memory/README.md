# RAM with a 200K-token conversation

The current README graph uses the newer [400K-token RSS benchmark](400k/).

This benchmark resumes the same synthetic conversation in Phi, Pi, and Claude
Code, then measures memory while each application waits for input. It does not
send a prompt or measure model generation.

## Results

Median of three fresh launches per scenario; all figures are MiB. Both RSS
and PSS are retained for this earlier run.

| Agent | Empty PSS | 200K-chat PSS | Empty RSS | 200K-chat RSS |
| --- | ---: | ---: | ---: | ---: |
| Phi | 4.8 | **7.1** | 18.3 | 20.7 |
| Pi 1.5.0 | 88.8 | **131.5** | 92.7 | 135.3 |
| Claude Code 2.1.319 | 170.2 | **176.3** | 245.7 | 255.1 |

Across individual launches, loaded PSS ranged from 7.0–7.1 MiB for Phi,
130.4–133.2 MiB for Pi, and 172.6–179.1 MiB for Claude Code. PSS depends on
page sharing with other running processes, not just an application's private
allocations. These are measurements on one machine, not universal requirements.
Codex was not included in this run.

Machine/build details: [`environment.json`](environment.json). Per-trial process
lists and memory samples: [`trials.json`](trials.json). Unrounded medians:
[`summary.json`](summary.json). The completed run's generated transcripts and
terminal logs are under `/tmp/phi-chat-memory-final` on the measurement machine;
that temporary directory is not a permanent archive.

## Workload

- 100 user/assistant turns: 200 messages, each containing exactly 1,000 text
  tokens under `tiktoken`'s `cl100k_base` encoding.
- Identical text in all three applications, serialized into their native JSONL
  transcript formats: Phi v2 items, Pi v3 message entries, and Claude Code's
  parent-linked user/assistant records.
- Plain repeated prose about a synthetic coding project; no tools, images,
  thinking blocks, or actual personal conversation content.
- The final assistant message ends with `BENCHMARK HISTORY LOADED`. Loaded
  trials fail unless that marker appears in the application's terminal output.
- “200K” refers to the sum of the text payloads under a common reference
  tokenizer. It is **not** an exact provider-specific context count and excludes
  system instructions, tool definitions, and message framing. Recorded usage
  fields are synthetic metadata, not API-reported token counts.

## Measurement

Each trial uses a fresh temporary HOME, config, state, cache, and workspace.
Local authentication files are copied into the temporary profile so the real
interactive UI can start; those private copies are removed when the trial ends.
Real chats and user settings are not modified. No prompts are submitted.
Claude's workspace trust screen is confirmed only for the empty synthetic
workspace, through its normal UI; no permission-bypass flags are used.

The PTY has 30 rows and 100 columns. Terminal output is continuously drained.
After 15 seconds from launch, process memory is sampled approximately every
300 ms for two seconds. Three fresh launches are run for each agent/scenario.
The summary takes the median sample within each trial, then the median trial.
The fixed trial order is Phi empty/loaded, Pi empty/loaded, Claude empty/loaded;
launches are sequential, with no cache dropping.

The primary metric is **PSS** from Linux `/proc/PID/smaps_rollup`, summed over
all processes inheriting a unique trial environment marker. PSS apportions
shared pages rather than counting each shared mapping in full. RSS is also
retained; it is a different metric and should not be substituted silently.
The terminal emulator and benchmark runner are excluded. Pre-existing services
without the trial marker are excluded; process names/PIDs are retained in the
raw results. Phi is launched with `--no-host`, so its shared chat-host process
is not part of this measurement.

Pi runs with `--offline`. Claude telemetry, auto-updating and nonessential
network traffic are disabled. These are otherwise isolated default profiles,
not the normal user profiles used for the older idle benchmark in
[`../memory`](../memory). Compare loaded RAM to **this benchmark's** empty
baseline, not that older baseline. Phi's empty UI includes its default welcome
animation. Results describe resumed text-only conversations, not peak RAM or a
representative tool-heavy coding workload.

## Reproduce

From the repository root, with the three agents installed and authenticated:

```bash
python3 -m venv /tmp/phi-chat-memory-venv
/tmp/phi-chat-memory-venv/bin/pip install tiktoken==0.14.0
/tmp/phi-chat-memory-venv/bin/python tests/test-chat-memory.py
/tmp/phi-chat-memory-venv/bin/python scripts/benchmark-chat-memory.py \
  --output /tmp/phi-chat-memory-results --trials 3 --settle 15
```

The script currently targets the local managed Pi installation at
`~/.local/bin/pi`, Claude Code at `~/.local/bin/claude`, and this repository's
`./phi`. Pi's managed Node runtime is expected at
`~/.local/share/pi-node/current/bin`.

`tiktoken` may download the public encoding table on first use; no transcript
content is sent to a tokenizer service. No model token-counting API is used.

The output directory contains the native synthetic transcripts, per-trial
terminal logs and results, plus `environment.json`, `trials.json`, and
`summary.json`. The JSON measurement files are retained here; large generated
transcripts/logs are reproducible and stay outside the repository. Inspect
terminal logs before publishing a new run, especially for empty trials where
there is no history sentinel.
