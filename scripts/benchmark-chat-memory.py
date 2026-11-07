#!/usr/bin/env python3
"""Measure Linux RSS/PSS after resuming identical synthetic chats.

Requires tiktoken. Does not submit prompts. Uses isolated HOME/config/state,
local credential copies (deleted after each trial), and a drained 30x100 PTY.
Run with --trials 1 first and inspect terminal logs for successful loading.
"""
import argparse
import fcntl
import hashlib
import json
import os
import platform
from datetime import datetime, timezone
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import statistics
import struct
import subprocess
import tempfile
import termios
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
HOME = Path.home()
SENTINEL = "BENCHMARK HISTORY LOADED"


def dump_lines(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(r) + "\n" for r in records))


def payloads(total_tokens=200000):
    import tiktoken
    enc = tiktoken.get_encoding("cl100k_base")
    paragraph = (
        "The synthetic project has a small parser, a queue, and a local cache. "
        "Each worker reads a record, checks its length, and writes a result. "
        "The tests cover empty inputs, repeated keys, and interrupted writes. "
        "Keep the public interface stable and document the error handling.\n"
    )
    source = enc.encode(paragraph * 100)
    if total_tokens <= 0 or total_tokens % 2000:
        raise ValueError('Token count must be a positive multiple of 2000 (complete turns).')
    message_count = total_tokens // 1000
    texts = []
    for i in range(message_count):
        suffix = "\n" + (SENTINEL if i == message_count - 1 else f"Synthetic message {i + 1} complete.")
        target = 1000 - len(enc.encode(suffix))
        text = enc.decode(source[:target]) + suffix
        # Token boundaries across the join can merge; correct using a known token.
        while len(enc.encode(text)) < 1000:
            text = " x" + text
        while len(enc.encode(text)) > 1000:
            target -= 1
            text = enc.decode(source[:target]) + suffix
        assert len(enc.encode(text)) == 1000
        texts.append(text)
    assert sum(len(enc.encode(t)) for t in texts) == total_tokens
    return texts


def transcripts(directory, texts, workspace):
    sid = str(uuid.uuid4())
    stamp = "2026-01-01T00:00:00.000Z"
    phi = [{"type": "session", "time": 1767225600, "data": {
        "format": "phi-transcript", "version": 2, "model": "gpt-6.1-sol",
        "effort": "off", "cwd": str(workspace)}}]
    pi = [{"type": "session", "version": 3, "id": sid, "timestamp": stamp,
           "cwd": str(workspace)}]
    claude = []
    previous = None
    for i, text in enumerate(texts):
        role = "user" if i % 2 == 0 else "assistant"
        ident = str(uuid.uuid4())
        phi.append({"type": "item", "time": 1767225600, "id": i + 1, "parent": i,
                    "data": {"role": role, "content": text} if role == "user" else {
                        "type": "message", "id": f"msg_bench_{i}", "role": role,
                        "status": "completed", "phase": "final_answer",
                        "content": [{"type": "output_text", "text": text}]}})
        message = {"role": role, "content": [{"type": "text", "text": text}],
                   "timestamp": 1767225600000}
        if role == "assistant":
            message.update(api="openai-responses", provider="openai", model="gpt-6.1-sol",
                           stopReason="stop", usage={"input": i * 1000, "output": 1000,
                           "cacheRead": 0, "cacheWrite": 0, "totalTokens": (i + 1) * 1000,
                           "cost": {"input": 0, "output": 0, "cacheRead": 0,
                                    "cacheWrite": 0, "total": 0}})
        pi.append({"type": "message", "id": ident, "parentId": previous,
                   "timestamp": stamp, "message": message})
        cm = {"role": role, "content": text if role == "user" else [
            {"type": "text", "text": text}]}
        if role == "assistant":
            cm.update(id=f"msg_bench_{i}", type="message", model="claude-opus-4-6",
                      stop_reason="end_turn", stop_sequence=None,
                      usage={"input_tokens": i * 1000, "output_tokens": 1000})
        claude.append({"parentUuid": previous, "isSidechain": False, "type": role,
                       "message": cm, "uuid": ident, "timestamp": stamp,
                       "userType": "external", "cwd": str(workspace),
                       "sessionId": sid, "version": "2.1.319", "gitBranch": ""})
        previous = ident
    paths = {}
    for name, records in [("phi", phi), ("pi", pi), ("claude", claude)]:
        paths[name] = directory / f"{name}.jsonl"
        dump_lines(paths[name], records)
    return paths, sid


def codex_transcript(path, texts, workspace, sid):
    stamp = datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')
    records = []

    def event(kind, payload):
        records.append({'timestamp': stamp, 'ordinal': len(records), 'type': kind, 'payload': payload})

    event('session_meta', {'id': sid, 'session_id': sid, 'timestamp': stamp,
        'cwd': str(workspace), 'originator': 'codex-tui', 'cli_version': '0.166.0',
        'source': 'cli', 'thread_source': 'user', 'model_provider': 'openai',
        'base_instructions': {'text': ''}})
    for i, text in enumerate(texts):
        role = 'user' if i % 2 == 0 else 'assistant'
        if role == 'user':
            turn = str(uuid.uuid4())
            event('event_msg', {'type': 'task_started', 'turn_id': turn, 'model_context_window': 475000})
            event('event_msg', {'type': 'user_message', 'message': text,
                'images': [], 'local_images': [], 'text_elements': []})
        message = {'type': 'message', 'id': f'msg_bench_{i}', 'role': role,
            'content': [{'type': 'input_text' if role == 'user' else 'output_text', 'text': text}]}
        if role == 'assistant':
            message['phase'] = 'final_answer'
        event('response_item', message)
        if role == 'assistant':
            event('event_msg', {'type': 'agent_message', 'message': text, 'phase': 'final_answer'})
            event('event_msg', {'type': 'task_complete', 'turn_id': turn, 'last_agent_message': text})
    dump_lines(path, records)


def claude_project_name(path):
    return re.sub(r'[^a-zA-Z0-9]', '-', str(path))


def process_memory(marker):
    processes = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if ("PHI_MEMORY_TRIAL=" + marker).encode() not in (entry / 'environ').read_bytes().split(b'\0'):
                continue
            values = {}
            for line in (entry / 'smaps_rollup').read_text().splitlines():
                if line.startswith(('Pss:', 'Rss:')):
                    key, value, _ = line.split()
                    values[key[:-1]] = int(value)
            processes.append({"pid": int(entry.name), "name": (entry / 'comm').read_text().strip(), **values})
        except (OSError, ValueError):
            pass
    return {"pss_kib": sum(p.get('Pss', 0) for p in processes),
            "rss_kib": sum(p.get('Rss', 0) for p in processes), "processes": processes}


def copy_private(source, destination):
    if source.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o600)


def trial(agent, scenario, index, args, texts):
    marker = str(uuid.uuid4())
    artifact = args.output / f"{agent}-{scenario}-{index}"
    artifact.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='phi-chat-memory-') as tmp:
        base = Path(tmp)
        home, work = base / 'home', base / 'work'
        home.mkdir(); work.mkdir()
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(('PHI_', 'CLAUDE', 'ANTHROPIC', 'OPENAI', 'CODEX', 'PI_')):
                env.pop(key)
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
                   XDG_STATE_HOME=str(home / '.local/state'), XDG_CACHE_HOME=str(home / '.cache'),
                   XDG_DATA_HOME=str(home / '.local/share'), TERM='xterm-256color',
                   COLORTERM='truecolor', PHI_MEMORY_TRIAL=marker,
                   PI_CODING_AGENT_DIR=str(home / '.pi/agent'),
                   DISABLE_TELEMETRY='1', DISABLE_AUTOUPDATER='1',
                   CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1', PI_OFFLINE='1')
        env['PATH'] = str(HOME / '.local/share/pi-node/current/bin') + ':' + env['PATH']
        copy_private(HOME / '.config/phi/auth.json', home / '.config/phi/auth.json')
        copy_private(HOME / '.pi/agent/auth.json', home / '.pi/agent/auth.json')
        copy_private(HOME / '.pi/agent/models-store.json', home / '.pi/agent/models-store.json')
        copy_private(HOME / '.claude/.credentials.json', home / '.claude/.credentials.json')
        # Onboarding preferences only, not real project histories or trust entries.
        (home / '.claude.json').write_text(json.dumps({"hasCompletedOnboarding": True,
                                                     "theme": "dark", "lastOnboardingVersion": "2.1.319"}))
        (home / '.pi/agent/settings.json').write_text(json.dumps({
            "lastChangelogVersion": "1.5.0", "defaultProvider": "openai",
            "defaultModel": "gpt-6.1-sol"}))
        paths, sid = transcripts(artifact, texts, work)
        if agent == 'phi':
            command = [str(ROOT / 'phi'), '--no-host']
            if scenario == 'loaded': command += ['--session', str(paths['phi'])]
        elif agent == 'pi':
            command = [str(HOME / '.local/bin/pi'), '--offline']
            if scenario == 'loaded': command += ['--session', str(paths['pi'])]
        elif agent == 'codex':
            codex = home / '.codex'
            codex.mkdir(parents=True)
            env['CODEX_HOME'] = str(codex)
            copy_private(HOME / '.codex/auth.json', codex / 'auth.json')
            copy_private(HOME / '.codex/models_cache.json', codex / 'models_cache.json')
            command = [str(HOME / '.local/bin/codex'), '--no-daemon']
            if scenario == 'loaded':
                now = datetime.now(timezone.utc)
                name = f"rollout-{now.strftime('%Y-%m-%dT%H-%M-%S')}-{sid}.jsonl"
                path = codex / 'sessions' / now.strftime('%Y/%m/%d') / name
                codex_transcript(path, texts, work, sid)
                shutil.copyfile(path, artifact / 'codex.jsonl')
                command += ['resume', sid]
        else:
            command = [str(HOME / '.local/bin/claude')]
            if scenario == 'loaded':
                project = home / '.claude/projects' / claude_project_name(work)
                project.mkdir(parents=True)
                shutil.copyfile(paths['claude'], project / f'{sid}.jsonl')
                command += ['--resume', sid]
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
        def terminal_setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        proc = subprocess.Popen(command, cwd=work, env=env, stdin=slave, stdout=slave,
                                stderr=slave, preexec_fn=terminal_setup)
        os.close(slave)
        output = bytearray()
        samples = []
        interactions = []
        started = time.monotonic()
        last_sample = 0
        confirmed_trust = False
        trust_confirm_at = None
        trust_select_at = None
        try:
            while time.monotonic() - started < args.settle + 2:
                if select.select([master], [], [], 0.1)[0]:
                    try: chunk = os.read(master, 65536)
                    except OSError: break
                    if not chunk: break
                    output.extend(chunk)
                    if agent != 'claude':
                        if b'\x1b[6n' in chunk: os.write(master, b'\x1b[1;1R')
                        if b'\x1b[c' in chunk: os.write(master, b'\x1b[?1;2c')
                    # Confirm only the isolated synthetic workspace when asked.
                    tail = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', bytes(output[-16000:])).lower()
                    compact = re.sub(rb'\s+', b'', tail)
                    if not confirmed_trust and b'yes,itrustthisfolder' in compact:
                        trust_select_at = time.monotonic() + 3
                        interactions.append('Confirmed trust for isolated synthetic workspace')
                        confirmed_trust = True
                if trust_select_at is not None and time.monotonic() >= trust_select_at:
                    os.write(master, b'\x1b[B')
                    trust_select_at = None
                    trust_confirm_at = time.monotonic() + 0.5
                if trust_confirm_at is not None and time.monotonic() >= trust_confirm_at:
                    os.write(master, b'\r')
                    trust_confirm_at = None
                elapsed = time.monotonic() - started
                if elapsed >= args.settle and elapsed - last_sample >= 0.3:
                    samples.append(process_memory(marker)); last_sample = elapsed
                if proc.poll() is not None: break
        finally:
            running = proc.poll() is None
            try: os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError: pass
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL); proc.wait()
            os.close(master)
        (artifact / 'terminal.log').write_bytes(output)
        result = {"agent": agent, "scenario": scenario, "trial": index,
                  "command": command, "running_at_sample": running,
                  "sentinel_seen": SENTINEL.replace(' ', '').encode() in re.sub(
                      rb'\s+', b'', re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', bytes(output))),
                  "interactions": interactions,
                  "samples": samples,
                  "pss_mib": statistics.median(s['pss_kib'] for s in samples) / 1024 if samples else None,
                  "rss_mib": statistics.median(s['rss_kib'] for s in samples) / 1024 if samples else None}
        (artifact / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: v for k, v in result.items() if k not in ['samples', 'command']}), flush=True)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--tokens', type=int, default=200000)
    parser.add_argument('--settle', type=float, default=12)
    parser.add_argument('--agents', nargs='+', choices=['phi', 'pi', 'claude', 'codex'], default=['phi', 'pi', 'claude'])
    parser.add_argument('--scenarios', nargs='+', default=['empty', 'loaded'])
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    texts = payloads(args.tokens)
    metadata = {"date_utc": datetime.now(timezone.utc).isoformat(),
                "platform": platform.platform(),
                "phi_binary_sha256": hashlib.sha256((ROOT / 'phi').read_bytes()).hexdigest(),
                "pi_version": subprocess.check_output([str(HOME / '.local/bin/pi'), '--version'],
                    env={**os.environ, 'PATH': str(HOME / '.local/share/pi-node/current/bin') + ':' + os.environ['PATH']},
                    text=True).strip(),
                "claude_version": subprocess.check_output([str(HOME / '.local/bin/claude'), '--version'], text=True).strip(),
                "codex_version": subprocess.check_output([str(HOME / '.local/bin/codex'), '--version'], text=True).strip() if 'codex' in args.agents else None,
                "backend_modes": {"phi": "--no-host", "codex": "--no-daemon (local backend)"},
                "content_tokens": args.tokens, "tokenizer": "tiktoken cl100k_base",
                "messages": len(texts), "turns": len(texts) // 2, "tokens_per_message": 1000,
                "payload_sha256": hashlib.sha256(''.join(texts).encode()).hexdigest(),
                "terminal": "30 rows x 100 columns", "settle_seconds": args.settle,
                "metric": "Median summed process-tree RSS; PSS also retained",
                "limitations": ["Text payload only; excludes framing/system/tool tokens.",
                    "Common reference tokenizer, not model-specific token counts.",
                    "Resumed history size, not a claim that the selected model supports this inference context.",
                    "Synthetic text-only history, not a tool-heavy coding session.",
                    "Isolated default profiles; not comparable to older normal-profile idle benchmark.",
                    "No model request submitted; not generation peak memory.",
                    "Check terminal logs for successful resume before publishing numbers."]}
    (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')
    results = []
    for index in range(args.trials):
        for agent in args.agents:
            for scenario in args.scenarios:
                results.append(trial(agent, scenario, index, args, texts))
                (args.output / 'trials.json').write_text(json.dumps(results, indent=2) + '\n')
                result = results[-1]
                if not result['running_at_sample'] or not result['samples'] or (
                        scenario == 'loaded' and not result['sentinel_seen']):
                    raise RuntimeError(f'{agent} {scenario} did not reach a verified measurement state; inspect terminal.log')
    summary = {}
    for agent in args.agents:
        summary[agent] = {}
        for scenario in args.scenarios:
            group = [r for r in results if r['agent'] == agent and r['scenario'] == scenario]
            summary[agent][scenario] = {
                metric: statistics.median(r[metric] for r in group)
                for metric in ['pss_mib', 'rss_mib']}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
