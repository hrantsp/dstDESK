# dstDESK Agent Operating Guide

`dstDESK` is the desktop half of the dual-stream transcription pipeline. It receives
microphone and meeting audio as separate streams from the `dstORCH` browser extension,
records them, and will transcribe and display them as a live conversation.

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
- `src/App/` — `kobayashi`: command line, self-test, wiring.
- `tst/` — unit tests. They link `kobayashi_core` and Catch2 only, so they run with no
  event loop, no display, and no Qt libraries present.
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

End to end, without a browser:

```bash
./bin/Release/kobayashi --output out     # terminal 1
./bin/Release/kobayashi-sim --seconds 3 --gap # terminal 2
```

`kobayashi-sim` sends 440 Hz on the microphone stream and 660 Hz on the meeting stream, so
the result is checkable by ear: one clean tone per file means capture and routing are
correct.

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

### The timing model

`sampleIndex` is a position on a clock **shared by both streams**, taken from the
extension's single `AudioContext`. Two frames carrying the same index were captured in
the same render pass. Cross-stream ordering is therefore an integer comparison with no
correction term.

Do not reintroduce per-stream timing origins or wall-clock timestamps for ordering.
The transcript merge will be built on this property.

### Gap padding is not cosmetic

When frames are lost, `sampleIndex` jumps and `StreamRecorder` inserts the equivalent
silence. Writing frames back to back after a loss would shift everything following it
permanently, and the same shift would corrupt the word timings ordering depends on.

### Recordings stay playable

A RIFF header claiming zero bytes makes a file unreadable however much audio sits
behind it. The sizes are patched roughly once a second while recording, and `SIGINT`
and `SIGTERM` are handled so destructors run. Both exist because a killed process
previously left whole sessions unplayable. Do not remove either.

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
- whether both Release and Debug were built
- whether the end-to-end `kobayashi` + `kobayashi-sim` run was performed, and what the session
  summary reported for frames, gaps and padded samples
- anything not verified, and why

Cross-platform status is honest rather than assumed: the code is portable by
construction but has only ever been compiled and run on Linux. Windows and macOS
remain unverified, with OpenSSL availability the known risk — `kobayashi --selftest`
answers it in one run on each machine.

## Records And Generated Files

- `out/` holds recordings, one directory per accepted session. Disposable.
- `bin/` is the build tree in its entirety. Disposable.
- `src/Core/Protocol.hpp` is generated. Never edit, never commit.

## Agent Handoff

Leave the tree building warning-free in both configurations with all tests passing.
Record architectural decisions — including rejected alternatives and what they cost —
in `dstOMNI/DESIGN.md` rather than in commit messages alone.
