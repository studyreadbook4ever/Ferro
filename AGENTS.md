# Working with FERRO

## Making music

Use FERRO's agent CLI to compose or edit music. Discover the installed interface first:

```sh
ferro-agent help
ferro-agent schema
```

In an uninstalled source checkout, use `./ferro-agent` from the repository root. It runs the same interface as `./build/ferro agent`. Read [the agent guide](docs/AGENT_CLI.md) and use [the four-bar plan](examples/agent-four-bars.json) as an executable starting point.

- Do not edit `.ferro` JSON directly or use mouse coordinates for music editing when this CLI is available.
- CLI standard output is JSON. Check the exit status and `ok`; a failed command is not a successful edit.
- `new --project FILE` creates an empty song. Add `--demo` only when a demo is wanted.
- Inspect before editing. Build a version 1 JSON plan and validate it before applying. Keep the plan in a file and pass `--file`; use `--stdin` when streaming JSON.
- Use the exact revision returned by `inspect` as `--expect`. This is required for `apply --live`. If the revision is stale, inspect again and reconcile the intended changes before retrying.
- If a person is editing a numeric field in the GUI, live `apply` (without `--dry-run`), `open`, `new`, and `save` return `ui_edit_in_progress` while preserving their text and focus. Wait for the person to press Enter or finish editing, then inspect again. `--force` does not bypass this guard. `inspect`, `status`, and dry runs remain available.
- A plan is one transaction: every operation succeeds or none is applied. Live application makes one GUI Undo step and does not start playback. Offline application updates the named project file; the open GUI is updated only by live commands or by opening that file.
- Offline application refuses to change a file known to be open in the GUI. It also refuses if the GUI is busy and its current file cannot be determined. Use the live workflow, or wait for the GUI to respond and inspect again, so unsaved edits are preserved.
- Tracks, bars, and steps start at **1**. There are **6 tracks**, **16 steps per bar**, and **4 beats per bar**. Notes use MIDI integers 0–127 or names such as `C4`, `F#3`, and `Bb2`; `C4` is MIDI 60.
- Use UI units: velocity 0–127, level 0–150, pan −100–100, tone/decay/drive 0–100, and output/delay/bass_duck 0–100. Do not supply the normalized values from the internal project format.
- Address a phrase by its unique name or its numeric `id` from `inspect`, within the selected track. Numeric IDs also work when a human-created project contains duplicate names; never guess IDs. Editing a phrase affects every clip using it. Inspect its usages; use `phrase.clone` and retarget the intended clip before making an independent variation.
- `notes.set` adds or updates only the named pitches. Unnamed pitches remain. Omitted velocity retains an existing note's velocity; new notes default to 100. Omitted sound settings retain existing overrides; an explicit `null` clears that override and restores the track default. `notes.remove` deletes only the named, existing pitches. Do not use velocity 0 as deletion.
- Setting `track.set`'s `instrument` explicitly clears that track's sample and restores the selected built-in instrument. Omit `instrument` to preserve a loaded sample while changing other track settings.
- Notes have no duration or gate field. Use `decay` for the sound envelope. Unsupported fields, unknown flags, bad types, duplicate note positions, overlaps, and out-of-range values are errors; correct the plan instead of bypassing validation.
- Save live changes explicitly. Use `--force` only when replacing a file or discarding an unsaved live song is intended. Do not add it automatically to get past a refusal.
- Render a WAV and inspect the resulting project to verify the intended music. A successful edit does not prove that the arrangement contains audible notes or that the mix is balanced.

## Changing the program

FERRO is a C++17 / Qt 6 application. Rebuild after source changes:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Keep GUI editing, undo/redo, project migration, sample paths, and offline rendering working. Reuse the existing project validation and atomic save helpers. Music CLI edits must preserve the same project constraints as GUI edits. Update the executable schema, agent guide, and runnable plan when changing the CLI contract.
