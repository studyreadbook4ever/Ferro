#!/usr/bin/env python3
"""Exercise the installed CLI contract and an isolated offscreen GUI process.

Usage: python3 tests/agent_cli_tests.py /absolute/path/ferro /source/root
Only Python's standard library is required. Every project and session lives in
temporary directories; the user's GUI and music files are never addressed.
"""

import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import wave


if len(sys.argv) < 3:
    raise SystemExit("Usage: agent_cli_tests.py BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE_ROOT = Path(sys.argv[2]).resolve()
del sys.argv[1:3]
EXAMPLE = SOURCE_ROOT / "examples" / "agent-four-bars.json"


def plan(*operations):
    return {"version": 1, "operations": list(operations)}


def midi(pitch):
    if isinstance(pitch, int):
        return pitch
    semitones = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    accidental = pitch[1:2]
    offset = 1 if accidental == "#" else -1 if accidental == "b" else 0
    octave = int(pitch[2:] if offset else pitch[1:])
    return 12 * (octave + 1) + semitones[pitch[0].upper()] + offset


class AgentCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ferro-agent-e2e-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = os.environ.copy()
        self.env.pop("DISPLAY", None)
        self.env.pop("WAYLAND_DISPLAY", None)
        # A GUI application would fail to start with this platform name.
        self.env["QT_QPA_PLATFORM"] = "ferro-test-invalid-platform"
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
            ("XDG_RUNTIME_DIR", "runtime"),
        ):
            target = self.root / directory
            target.mkdir(mode=0o700)
            self.env[variable] = str(target)

    def cli(self, *arguments, expect=True, stdin=None, timeout=30):
        command = [str(BINARY), "agent", *(str(value) for value in arguments)]
        result = subprocess.run(
            command,
            cwd=self.root,
            env=self.env,
            input=stdin,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        context = f"command={command!r}\nstdout={result.stdout}\nstderr={result.stderr}"
        try:
            reply = json.loads(result.stdout)
        except (ValueError, TypeError) as error:
            self.fail(f"Expected exactly one JSON response: {error}\n{context}")
        self.assertIsInstance(reply, dict, context)
        self.assertIs(type(reply.get("ok")), bool, context)
        self.assertEqual(result.returncode == 0, reply["ok"], context)
        if expect is not None:
            self.assertEqual(reply["ok"], expect, context)
        if not reply["ok"]:
            self.assertIsInstance(reply.get("error"), dict, context)
            self.assertIsInstance(reply["error"].get("code"), str, context)
            self.assertTrue(reply["error"].get("message"), context)
        return reply

    def write_plan(self, value, name="plan.json"):
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def new_project(self, name="song.ferro"):
        path = self.root / name
        self.cli("new", "--project", path)
        return path

    def assert_audio(self, path, seconds):
        with wave.open(str(path), "rb") as audio:
            self.assertEqual(audio.getnchannels(), 2)
            self.assertEqual(audio.getframerate(), 48000)
            self.assertEqual(audio.getsampwidth(), 3)
            self.assertEqual(audio.getcomptype(), "NONE")
            self.assertAlmostEqual(audio.getnframes() / 48000, seconds, delta=1 / 48000)
            raw = audio.readframes(audio.getnframes())
        values = [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, len(raw), 3)]
        peak = max(abs(value) for value in values)
        rms = math.sqrt(sum(value * value for value in values) / len(values)) / 8388608
        self.assertGreater(peak, 1000, "The exported arrangement must contain audio")
        self.assertGreater(rms, 0.001)
        self.assertLess(peak, 8388607, "The bundled example should not clip")

    def test_discovery_and_new_song_defaults(self):
        help_reply = self.cli("help")
        self.assertTrue(help_reply["commands"])
        self.assertEqual(self.cli("version")["protocol"], 1)
        schema = self.cli("schema")["schema"]
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["properties"]["version"]["const"], 1)
        operations = schema["properties"]["operations"]["items"]["oneOf"]
        self.assertIn("notes.set", [item["properties"]["op"]["const"] for item in operations])

        path = self.new_project()
        blank = self.cli("inspect", "--project", path)["data"]
        self.assertEqual(blank["bars"], 16)
        self.assertEqual(blank["tempo"], 120)
        self.assertEqual([t["instrument"] for t in blank["tracks"]], ["Kick", "Bass", "Saw", "Lead", "Clap", "Hats"])
        self.assertTrue(all(not t["phrases"] and not t["clips"] for t in blank["tracks"]))
        previous = path.read_bytes()
        self.cli("new", "--project", path, expect=False)
        self.assertEqual(path.read_bytes(), previous)

        showcase_path = self.root / "showcase.ferro"
        showcase = self.cli("new", "--project", showcase_path, "--demo")["data"]
        self.assertEqual((showcase["title"], showcase["tempo"], showcase["bars"], showcase["tail"]),
                         ("Neon Signal - 2011", 126, 48, 3))
        self.assertAlmostEqual(showcase["duration_seconds"], 48 * 240 / 126 + 3)
        self.assertEqual(json.loads(showcase_path.read_text()),
                         json.loads((SOURCE_ROOT / "examples" / "Neon-Signal-2011.ferro").read_text()))

    def test_runnable_example_transaction_inspection_and_pcm_export(self):
        path = self.new_project()
        original = path.read_bytes()
        revision = self.cli("inspect", "--project", path)["revision"]
        self.cli("validate", "--project", path, "--file", EXAMPLE)
        self.cli("validate", "--project", path, "--stdin", stdin=EXAMPLE.read_text())
        dry = self.cli("apply", "--project", path, "--file", EXAMPLE, "--expect", revision, "--dry-run")
        self.assertTrue(dry["dry_run"])
        self.assertEqual(path.read_bytes(), original)
        applied = self.cli("apply", "--project", path, "--file", EXAMPLE, "--expect", revision)
        inspected = self.cli("inspect", "--project", path)
        self.assertEqual(inspected["revision"], applied["revision"])
        self.assertEqual(dry["revision"], applied["revision"])
        self.assertNotEqual(inspected["revision"], revision)
        song = inspected["data"]
        self.assertEqual((song["tempo"], song["bars"]), (126, 4))

        expected = {}
        for op in json.loads(EXAMPLE.read_text())["operations"]:
            if op["op"] == "notes.set":
                expected[op["track"]] = {
                    (group["bar"], group["step"], midi(pitch)): group.get("velocity", 100)
                    for group in op["notes"] for pitch in group["pitches"]
                }
        for track in song["tracks"]:
            self.assertEqual(len(track["phrases"]), 1)
            self.assertEqual(len(track["clips"]), 1)
            phrase, clip = track["phrases"][0], track["clips"][0]
            self.assertEqual((clip["bar"], clip["bars"], clip["phrase"]), (1, 4, phrase["name"]))
            self.assertIn(clip["id"], phrase["used_by"])
            actual = {(n["bar"], n["step"], n["midi"]): n["velocity"] for n in phrase["notes"]}
            self.assertEqual(actual, expected[track["track"]])
        # Three pitches at one step prove that this workflow really creates chords.
        self.assertEqual(sum(n["bar"] == 1 and n["step"] == 1
                             for n in song["tracks"][2]["phrases"][0]["notes"]), 3)

        output = self.root / "example.wav"
        result = self.cli("render", "--project", path, "--output", output, timeout=90)
        self.assertEqual((result["sample_rate"], result["channels"], result["bits"]), (48000, 2, 24))
        self.assert_audio(output, 4 * 240 / 126 + 2)
        previous = output.read_bytes()
        self.cli("render", "--project", path, "--output", output, expect=False)
        self.assertEqual(output.read_bytes(), previous)

    def test_strict_options_and_input_failures(self):
        path = self.new_project()
        good = self.write_plan(plan({"op": "song.set", "tempo": 127}))
        cases = [
            ("not-a-command",),
            ("inspect", "--project"),
            ("inspect", "--project", path, "--project", path),
            ("inspect", "--project", path, "--typo"),
            ("inspect", "--live", "--project", path),
            ("apply", "--project", path),
            ("apply", "--project", path, "--file", good, "--stdin"),
            ("apply", "--project", path, "--stdin", "--stdin"),
            ("new", "--project", path, "--force", "--force"),
            ("schema", "--live"),
            ("apply", "--project", path, "--file", good, "--expect", "guessed"),
            ("open", "--project", path),
        ]
        previous = path.read_bytes()
        for args in cases:
            with self.subTest(arguments=args):
                self.cli(*args, expect=False)
                self.assertEqual(path.read_bytes(), previous)
        bad_json = self.root / "malformed.json"
        bad_json.write_text('{"version": 1, "operations": [')
        self.cli("apply", "--project", path, "--file", bad_json, expect=False)
        self.cli("apply", "--project", path, "--stdin", stdin="[]", expect=False)
        self.assertEqual(path.read_bytes(), previous)

    def test_bad_notes_late_failure_and_stale_revision_preserve_files(self):
        path = self.new_project()
        setup = self.write_plan(plan({"op": "phrase.create", "track": 3, "name": "Chord", "bars": 1}))
        self.cli("apply", "--project", path, "--file", setup)
        original = path.read_bytes()
        revision = self.cli("inspect", "--project", path)["revision"]
        bad_groups = [
            {"bar": 1, "step": 1, "pitches": [60], "duration": 4},
            {"bar": 1, "step": 0, "pitches": [60]},
            {"bar": 1, "step": 1.5, "pitches": [60]},
            {"bar": 1, "step": 1, "pitches": [128]},
            {"bar": 1, "step": 1, "pitches": [60.5]},
            {"bar": 1, "step": 1, "pitches": [60, "C4"]},
        ]
        for group in bad_groups:
            with self.subTest(group=group):
                invalid_plan = self.write_plan(plan(
                    {"op": "song.set", "title": "Must roll back"},
                    {"op": "notes.set", "track": 3, "phrase": "Chord", "notes": [group]},
                ))
                error = self.cli("apply", "--project", path, "--file", invalid_plan, expect=False)
                self.assertEqual(error["error"]["operation"], 2)
                self.assertEqual(path.read_bytes(), original)
                self.assertEqual(self.cli("inspect", "--project", path)["revision"], revision)
        duplicate_groups = self.write_plan(plan({
            "op": "notes.set", "track": 3, "phrase": "Chord", "notes": [
                {"bar": 1, "step": 1, "pitches": [60]},
                {"bar": 1, "step": 1, "pitches": ["C4"]},
            ],
        }))
        self.cli("apply", "--project", path, "--file", duplicate_groups, expect=False)
        self.assertEqual(path.read_bytes(), original)

        update = self.write_plan(plan({"op": "song.set", "tempo": 127}))
        self.cli("apply", "--project", path, "--file", update, "--expect", revision)
        changed = path.read_bytes()
        self.cli("apply", "--project", path, "--file", update, "--expect", revision, expect=False)
        self.assertEqual(path.read_bytes(), changed)

    def test_sample_paths_survive_mix_edits_and_failed_render_keeps_output(self):
        path = self.new_project()
        sample = self.root / "samples" / "fixture.wav"
        sample.parent.mkdir()
        with wave.open(str(sample), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(48000)
            audio.writeframes(b"\x00\x00" * 480)
        # Native fixture construction is intentional: importing samples is a GUI
        # feature, whereas the CLI must preserve existing project sample paths.
        fixture = json.loads(path.read_text())
        fixture["songBars"] = 1
        fixture["tracks"][0]["sample"] = "samples/fixture.wav"
        path.write_text(json.dumps(fixture))
        edit = self.write_plan(plan({"op": "track.set", "track": 1, "level": 41, "pan": -12}))
        self.cli("apply", "--project", path, "--file", edit)
        inspected = self.cli("inspect", "--project", path)["data"]["tracks"][0]
        self.assertEqual(Path(inspected["sample"]), sample)
        self.assertEqual(inspected["level"], 41)
        self.assertEqual(json.loads(path.read_text())["tracks"][0]["sample"], "samples/fixture.wav")

        sample.unlink()
        output = self.root / "preserve.wav"
        sentinel = b"existing output must survive a failed sample render"
        output.write_bytes(sentinel)
        self.cli("render", "--project", path, "--output", output, "--force", expect=False, timeout=90)
        self.assertEqual(output.read_bytes(), sentinel)

    def test_live_session_isolated_from_the_desktop(self):
        # A unique RuntimeLocation changes the socket endpoint, and a unique
        # DataLocation changes the GUI lock/autosave paths. No request can reach
        # a real desktop session even when these tests run while FERRO is open.
        absent = self.cli("status", "--live", expect=False)
        self.assertEqual(absent["error"]["code"], "not_running")
        gui_env = self.env.copy()
        gui_env["QT_QPA_PLATFORM"] = "offscreen"
        logs = self.root / "isolated-gui.log"
        with logs.open("w+", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(BINARY)], cwd=self.root, env=gui_env,
                stdout=log, stderr=log, text=True,
            )
            try:
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        log.seek(0)
                        self.fail(f"Isolated GUI exited with {process.returncode}: {log.read()}")
                    status = self.cli("status", "--live", expect=None, timeout=3)
                    if status["ok"]:
                        break
                    if time.monotonic() >= deadline:
                        log.seek(0)
                        self.fail(f"Isolated GUI did not become ready: {status!r}\n{log.read()}")
                    time.sleep(0.1)

                initial = self.cli("inspect", "--live")
                self.assertEqual(initial["data"]["title"], "Neon Signal - 2011")
                self.assertEqual(initial["data"]["bars"], 48)
                self.assertFalse(initial["dirty"])
                self.assertFalse(initial["playing"])
                self.assertFalse(initial["play_pending"])
                self.cli("new", "--live")
                blank = self.cli("inspect", "--live")
                original_revision = blank["revision"]

                self.cli("apply", "--live", "--file", EXAMPLE, expect=False)
                self.assertEqual(self.cli("inspect", "--live")["revision"], original_revision)
                self.cli("apply", "--live", "--file", EXAMPLE, "--expect", original_revision, "--dry-run")
                preview = self.cli("inspect", "--live")
                self.assertEqual(preview["revision"], original_revision)
                self.assertFalse(preview["dirty"])
                self.cli("apply", "--live", "--file", EXAMPLE, "--expect", original_revision)
                current = self.cli("inspect", "--live")
                self.assertTrue(current["dirty"])
                self.assertFalse(current["playing"])
                self.assertFalse(current["play_pending"])
                self.assertEqual(current["path"], "")
                self.assertEqual(current["data"]["bars"], 4)
                self.cli("apply", "--live", "--file", EXAMPLE, "--expect", original_revision, expect=False)
                self.assertEqual(self.cli("inspect", "--live")["revision"], current["revision"])

                saved = self.root / "live-song.ferro"
                self.assertFalse(saved.exists(), "Live apply must not silently save a project")
                self.cli("save", "--live", "--output", saved)
                clean = self.cli("inspect", "--live")
                self.assertFalse(clean["dirty"])
                self.assertEqual(Path(clean["path"]), saved)
                saved_bytes = saved.read_bytes()

                # Agent save paths are explicit projects, and a legacy project
                # with a .wav suffix must never become its own render output.
                invalid_save = self.root / "not-a-project.wav"
                self.cli("save", "--live", "--output", invalid_save, expect=False)
                self.assertFalse(invalid_save.exists())
                self.assertEqual(self.cli("status", "--live")["path"], str(saved))
                legacy = self.root / "legacy-project.wav"
                legacy.write_bytes(saved_bytes)
                self.cli("open", "--live", "--project", legacy)
                collision = self.cli("render", "--live", "--output", legacy, "--force", expect=False)
                self.assertEqual(collision["error"]["code"], "OUTPUT_COLLISION")
                self.assertEqual(legacy.read_bytes(), saved_bytes)
                self.cli("open", "--live", "--project", saved)

                mix = self.write_plan(plan({"op": "song.set", "tempo": 127}))
                blocked = self.cli("apply", "--project", saved, "--file", mix, expect=False)
                self.assertEqual(blocked["error"]["code"], "PROJECT_OPEN_USE_LIVE")
                self.assertEqual(saved.read_bytes(), saved_bytes)
                output = self.root / "live-preview.wav"
                self.cli("render", "--live", "--output", output, timeout=90)
                self.assert_audio(output, 4 * 240 / 126 + 2)

                self.cli("apply", "--live", "--file", mix, "--expect", clean["revision"])
                dirty = self.cli("inspect", "--live")
                self.assertTrue(dirty["dirty"])
                self.cli("open", "--live", "--project", self.root / "missing.ferro", "--force", expect=False)
                self.cli("new", "--live", expect=False)
                preserved = self.cli("inspect", "--live")
                self.assertEqual(preserved["revision"], dirty["revision"])
                self.assertEqual(preserved["path"], dirty["path"])
                self.assertTrue(preserved["dirty"])
                self.assertEqual(saved.read_bytes(), saved_bytes)
                self.cli("save", "--live")
                self.cli("new", "--live", "--demo")
                restored = self.cli("inspect", "--live")
                self.assertEqual(restored["data"]["title"], "Neon Signal - 2011")
                self.assertEqual(restored["data"]["bars"], 48)
                self.assertFalse(restored["dirty"])
                self.assertFalse(restored["playing"])
                self.assertEqual(restored["path"], "")
            finally:
                # Only the process created by this test is terminated. Do not
                # match names, kill process groups, or contact desktop windows.
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
