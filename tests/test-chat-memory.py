#!/usr/bin/env python3
"""Run using the same tiktoken-enabled Python as the benchmark."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'chat_memory', Path(__file__).resolve().parents[1] / 'scripts/benchmark-chat-memory.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class TranscriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.texts = bench.payloads()

    def test_exact_token_count(self):
        import tiktoken
        enc = tiktoken.get_encoding('cl100k_base')
        self.assertEqual(len(self.texts), 200)
        self.assertEqual([len(enc.encode(t)) for t in self.texts], [1000] * 200)
        self.assertTrue(self.texts[-1].endswith(bench.SENTINEL))

    def test_400k_token_count(self):
        import tiktoken
        enc = tiktoken.get_encoding('cl100k_base')
        texts = bench.payloads(400000)
        self.assertEqual(len(texts), 400)
        self.assertEqual(sum(len(enc.encode(t)) for t in texts), 400000)
        self.assertTrue(texts[-1].endswith(bench.SENTINEL))

    def test_invalid_token_count(self):
        for count in [0, -2000, 3000]:
            with self.assertRaises(ValueError):
                bench.payloads(count)

    def test_codex_transcript(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'codex.jsonl'
            bench.codex_transcript(path, self.texts, Path(directory), 'test-session')
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual(rows[0]['payload']['id'], 'test-session')
            self.assertEqual([r['ordinal'] for r in rows], list(range(len(rows))))
            messages = [r['payload'] for r in rows if r['type'] == 'response_item']
            self.assertEqual([m['content'][0]['text'] for m in messages], self.texts)
            self.assertEqual([m['role'] for m in messages], ['user', 'assistant'] * 100)
            events = [r['payload'] for r in rows if r['type'] == 'event_msg']
            self.assertEqual(sum(e['type'] == 'task_started' for e in events), 100)
            self.assertEqual(sum(e['type'] == 'task_complete' for e in events), 100)

    def test_claude_project_name(self):
        self.assertEqual(bench.claude_project_name('/tmp/phi-chat-memory-abc_def/work'),
                         '-tmp-phi-chat-memory-abc-def-work')
        self.assertEqual(bench.claude_project_name('/home/cmcdo/sync/phi_public'),
                         '-home-cmcdo-sync-phi-public')

    def test_identical_content_and_parent_chains(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths, sid = bench.transcripts(root, self.texts, root / 'work')
            records = {name: [json.loads(line) for line in path.read_text().splitlines()]
                       for name, path in paths.items()}
            self.assertEqual(records['phi'][0]['data']['format'], 'phi-transcript')
            self.assertEqual(records['pi'][0]['version'], 3)
            previous_pi = previous_claude = None
            for i, text in enumerate(self.texts):
                phi = records['phi'][i + 1]
                pi = records['pi'][i + 1]
                claude = records['claude'][i]
                role = 'user' if i % 2 == 0 else 'assistant'
                self.assertEqual(phi['id'], i + 1)
                self.assertEqual(phi['parent'], i)
                self.assertEqual(pi['parentId'], previous_pi)
                self.assertEqual(claude['parentUuid'], previous_claude)
                self.assertEqual(claude['sessionId'], sid)
                for message in [phi['data'], pi['message'], claude['message']]:
                    self.assertEqual(message['role'], role)
                    content = message['content']
                    actual = content if isinstance(content, str) else content[0]['text']
                    self.assertEqual(actual, text)
                previous_pi, previous_claude = pi['id'], claude['uuid']


if __name__ == '__main__':
    unittest.main()
