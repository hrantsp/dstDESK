# dstDESK Agent Operating Guide

`dstDESK` is the desktop half of the dual-stream transcription pipeline. It receives
microphone and meeting audio as separate streams from the `dstORCH` browser extension,
records them, transcribes them, and displays them as a live conversation.

It also **owns the wire protocol**. Changes here can break the extension, so treat
`rec/` as a published interface rather than internal detail.

This guide applies to the whole repository.

## Project Shape

- `src/Core/` — the framework-free half: frame parsing and validation, byte order,
  WAV writing, gap padding. **Contains no Qt.**
- `src/IO/` — Qt-facing network code. Currently the WebSocket server; the
  transcription client belongs here too.
- `src/Sim/` — `kobayashi-sim`, a stand-in for the browser extension. A client of the
  protocol, not a feature of the application.
- `src/App/` — `kobayashi`: command line, self-test, wiring. The transcript is a
  model, a filter proxy and a delegate; do not reintroduce a widget per utterance,
  and see decision 24 for the measurements that settled it.
- `tst/` — checks, of four kinds.
  - `TestFrame`, `TestRecorder`, `TestTranscript` link `kobayashi_core` and Catch2 only,
    so they run with no event loop, no display, and no Qt libraries present.
  - `abuse.mjs` drives a real socket against a running server and is the only thing that
    checks the MUSTs in `rec/PROTOCOL.md` §5.3 are implemented. A change to the wire
    contract is not done until this passes.
  - `sessions.mjs` covers what happens *between* connections — a client displaced by a
    reconnecting one, and two sessions inside a single wall-clock second. It needs a
    server started with recording **on** and transcription pointed at `kobayashi-mockstt`,
    which is why it is a separate check from `abuse.mjs`: the faults it guards cannot
    happen in the `--no-record --no-transcribe` configuration that one runs in.
  - `BenchTranscript.cpp` guards a property rather than a behaviour: that appending to
    the transcript does not get slower as the transcript grows. See decision 24.
- `rec/` — the protocol source of truth, its generator, both specifications, and the
  Conan recipe for Qt.
- `bin/`, `out/` — build tree and recordings. Both generated, both disposable.

## Build And Test

From the workspace root, once per machine:

```bash
python3 -m venv .venv
.venv/bin/pip install -r dstOMNI/requirements.txt
export PATH="$PWD/.venv/bin:$PATH"
conan profile detect --force
conan create dstDESK/rec/qt-official        # downloads official Qt, ~1.6 GB, once
```

`dstOMNI/dst.py` wraps all of this — `python3 dstOMNI/dst.py setup|build|test|package`
— and is the path a reviewer follows. The commands below are what it runs, and are
what to use when working on this repository directly.

Then, in `dstDESK/`:

```bash
conan install . --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release
```

`conan-debug` exists alongside `conan-release`. Both must be warning-free.

```bash
python3 dstOMNI/dst.py build --sanitize   # a second tree in bin/Sanitize
python3 dstOMNI/dst.py test  --sanitize   # the whole suite against it
```

AddressSanitizer and UndefinedBehaviorSanitizer, in a build tree of their own so a
sanitized binary can never be packaged by forgetting to reconfigure. The run fails on any
report from any process it starts, including the detached ones whose stderr is otherwise
discarded — see decision 29 for why that needs two mechanisms rather than one. Anything
touching `Core/`, `IO/` or the receive path is not done until this passes.

`python3 dstOMNI/dst.py test` runs all of them, plus the browser wire check — it starts
the servers they need, so none of them depends on someone remembering to.

End to end, without a browser:

```bash
./bin/Release/kobayashi --output out     # terminal 1
./bin/Release/kobayashi-sim --seconds 3 --gap # terminal 2
```

`kobayashi-sim` sends 440 Hz on the microphone stream and 660 Hz on the meeting stream, so
the result is checkable by ear: one clean tone per file means capture and routing are
correct.

`kobayashi-mockstt` is a transcription service that drops connections on purpose, and
`--stt-endpoint` points the application at it. This is how the reconnect and merge paths
are exercised without a paid account; `dst.py test` runs two sessions against it.

Anything touching `SttClient` or `TranscriptMerger` is not done until that passes — and a
new check there is not done until it has been shown to **fail** with the fault it guards
put back. Two of the three reconnect faults were of a kind no reading would have caught.

## Hard Contracts

### Generated files

`rec/protocol.json` is the single source of truth. Two files are generated from it and
**must never be hand-edited**:

- `src/Core/Protocol.hpp`
- `../dstORCH/src/generated/protocol.js`

Neither is committed. `rec/generate.py --check` fails a build against stale output, and
the generator validates the specification itself: header fields must be contiguous,
declared sizes must agree, and the header length must stay even so the `int16` payload
is not misaligned.

Changing the wire format means changing `rec/protocol.json` **and** `rec/PROTOCOL.md`
together, and bumping `protocolVersion` if the change is not backward compatible.

### Transcription owns its own clock

`SttClient` maps engine time onto the shared capture clock, and nothing outside it should
try to. Every chunk of audio is handed over with its position on that clock; gaps, and the
re-basing a reconnection needs, are settled there. A new connection's origin is the front
of whatever was buffered, not the next frame after it comes up — decision 25 has the
measurement that distinguishes those.

An unexpected drop marks the stream **stalled**, never closed: it may speak again, and
closing it discards everything the replacement produces.

### The timing model

`sampleIndex` is a position on a clock **shared by both streams**, taken from the
extension's single `AudioContext`. Two frames carrying the same index were captured in
the same render pass. Cross-stream ordering is therefore an integer comparison with no
correction term.

Do not reintroduce per-stream timing origins or wall-clock timestamps for ordering.
`Core/TranscriptMerger` is built on this property.

### Gap padding is not cosmetic, and it is bounded

When frames are lost, `sampleIndex` jumps and `StreamRecorder` inserts the equivalent
silence — into the recording **and** into the stream forwarded to the engine. Writing
frames back to back after a loss would shift everything following it permanently, and
the same shift would corrupt the word timings ordering depends on.

The padding is capped at thirty seconds per gap on both paths, and the cap is not
optional. `sampleIndex` arrives from the client, and padding writes the difference: one
frame claiming a position four billion samples ahead used to mean eight gigabytes of
silence on disk. Past the cap, do not pad, continue from the new position, and say so.

**The cap binds one frame and not one second, and that is a known limit rather than a
design.** It is applied per call and remembers nothing, so a client asking for a large gap
on every frame is not bounded by it at all — measured at 53 MB on disk and 1,711 s of
billable audio from 1.92 s of real audio. Decision 27 has the numbers and the shape of the
fix; do not close it by lowering the per-gap cap, which would break the genuine drops the
cap exists to pad.

### Recordings stay playable

A RIFF header claiming zero bytes makes a file unreadable however much audio sits
behind it. The sizes are patched roughly once a second while recording, and `SIGINT`
and `SIGTERM` are handled so destructors run. Both exist because a killed process
previously left whole sessions unplayable. Do not remove either.

### A fix is not finished until the same fault has been looked for one level up

Decision 28. Three separate fixes in this repository stopped at the edge of the case that
had been reproduced, and in every one the same fault was sitting one level above it: a
teardown race fixed synchronously and not asynchronously, a recording overwrite fixed for
the file and not the directory, a write bound fixed per frame and not per second.

So after the test goes green: name the class of the fault in one sentence, then go and
look for that class somewhere the reproduction did not reach. Record what the search
found, including when it found nothing. That search is part of the change, not a follow-up.

### Core stays framework-free

Nothing under `src/Core/` may include Qt. That is what keeps the suite runnable
without a display and, on Windows, without Qt DLLs anywhere near the test binary.

### Security defaults

The server binds loopback only, never `0.0.0.0`. Origin is settled during the HTTP
upgrade, not after the socket is accepted. Both are specified in `rec/PROTOCOL.md` §7.

## Code Style

- `.hpp` for headers, with `#ifndef` guards named after the full scope.
- Uppercase-starting file and directory names.
- `namespace DST { namespace DESK { namespace Core {` on one line; close with
  `} } } // namespace DST::DESK::Core`.
- Opening brace on its own line for multi-line bodies; single statements stay inline.
- Explicit values and an explicit underlying type on every enum. Enums are a struct
  wrapping an enum, with descriptions as static members — see `ParseError` and the
  generated `Stream`.
- Short names doubled — `pp`, `ss`, `cc`, `dd`, `ii`, `nn`, `vv` — so they can be
  searched for.
- System includes first, then local ones.
- Two-space indentation. Align related declarations into columns.

## Change Ownership

Use the smallest owning area:

- `src/Core/` for parsing, validation, byte order, recording, and later the transcript
  ordering model.
- `src/IO/` for the server and the transcription client.
- `src/Sim/` for the synthetic client.
- `src/App/` for command line and startup behaviour.
- `rec/` for the protocol, its generator, and the Qt recipe.
- `tst/` for tests.

Do not edit sibling repositories from a `dstDESK` change unless the protocol itself
moved, in which case `dstORCH` must be verified against it.

## Testing Expectations

Before claiming a change works, state:

- changed files and owning area
- exact build and test commands, and whether the tree was clean
- whether both Release and Debug were built, and whether `test --sanitize` was run
- what class the fault belonged to, and where else that class was looked for
- whether the end-to-end `kobayashi` + `kobayashi-sim` run was performed, and what the session
  summary reported for frames, gaps and padded samples
- anything not verified, and why

Cross-platform status is recorded per platform in `dstOMNI/README.md`, and that table
is the only place it is stated. Linux, Windows 10 and macOS on Apple silicon have each
run the full pipeline on hardware. A claim about a platform belongs in that table with
the run behind it, or nowhere — the same status written in two files is the same status
until one of them stops being updated.

## Records And Generated Files

- `out/` holds recordings, one directory per accepted session. Disposable.
- `bin/` is the build tree in its entirety. Disposable.
- `src/Core/Protocol.hpp` is generated. Never edit, never commit.

## Agent Handoff

Leave the tree building warning-free in both configurations with all tests passing.
Record architectural decisions — including rejected alternatives and what they cost —
in `dstOMNI/DESIGN.md` rather than in commit messages alone.
