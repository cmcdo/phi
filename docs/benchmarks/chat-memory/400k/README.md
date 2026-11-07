# 400K-token chat memory

RSS after resuming 400 messages (200 user/assistant turns), each containing
1,000 text tokens under `tiktoken`'s `cl100k_base` encoding. Identical text is
serialized into each application's native transcript format.

The setup and limitations are the same as the [200K run](../README.md): fresh
isolated profiles, a drained 30×100 terminal, three launches per scenario,
samples taken 15–17 seconds after launch, no submitted prompts. RSS is the
primary metric; PSS is retained in the raw results. Terminal memory is excluded.

This measures **loaded conversation history**, not model execution. The 400K
reference-token payload excludes system instructions and message framing.
It does not establish that the selected model supports a 400K inference window.
The content is generated, repeated text, not a real tool-heavy coding session.

## Results

Median of three launches per scenario, in MiB:

| Agent | Idle RSS | 400K-chat RSS | Idle PSS | 400K-chat PSS |
| --- | ---: | ---: | ---: | ---: |
| Phi | 18.3 | **22.4** | 5.0 | 9.0 |
| Pi 1.5.0 | 92.3 | **144.3** | 88.5 | 140.4 |
| Claude Code 2.1.319 | 246.0 | **255.7** | 171.2 | 178.5 |
| Codex 0.166.0 | 521.9 | **563.0** | 521.9 | 563.0 |

[Environment](environment.json) · [Raw samples](trials.json) · [Unrounded medians](summary.json)

All 18 trials completed. Each loaded trial displayed the final verification
marker. An earlier attempt failed because the Claude project-directory encoding
did not replace underscores; it was excluded and the full run was repeated
after fixing that lookup and adding a regression test.

## Codex

Codex was rerun with the **same measurement protocol**: three fresh launches
per scenario, an isolated profile, a continuously drained 30×100 PTY, and RSS
samples at 15–17 seconds. Its 400K text payload hash matches the other agents
exactly. All six trials completed; all loaded trials displayed the final marker.
No model requests were submitted.

It uses `--no-daemon` so its local backend is included, without attaching to a
pre-existing shared server. Phi similarly uses `--no-host`. All processes
inheriting the trial marker are counted; the terminal and runner are excluded.
The temporary CODEX_HOME produces a warning about helper PATH aliases; no tools
are executed in this workload. Codex's launches were performed separately from
the other agents, using the same script and sampling protocol.

Individual trial RSS medians ranged from 445.8–532.9 MiB idle and
558.4–584.1 MiB loaded. The chart uses the median of each set of three.

[Environment](codex-standard/environment.json) · [Raw samples](codex-standard/trials.json) ·
[Unrounded medians](codex-standard/summary.json).

The earlier **665.0 MiB** [live observation](codex-live.json) is retained only
as historical evidence. It is superseded in the chart by the standard-protocol
result, not averaged into it.

## Reproduce

```bash
/tmp/phi-chat-memory-venv/bin/python scripts/benchmark-chat-memory.py \
  --output /tmp/phi-chat-memory-400k-new --tokens 400000 --trials 3 --settle 15
```

For Codex, add `--agents codex` and choose a new output directory. For all four
agents together, use `--agents phi pi claude codex`.

See the parent directory for Python environment setup. Raw generated transcripts
and terminal logs remain in `/tmp/phi-chat-memory-400k-verified` (Phi, Pi, Claude)
and `/tmp/phi-codex-400k-standard` (Codex) on the measurement machine. These
temporary directories are not permanent archives.
