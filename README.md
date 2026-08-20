# dstDESK — Kobayashi

**Kobayashi** is the cross-platform C++/Qt desktop application of the dual-stream
transcription pipeline; `dstDESK` is the repository it lives in. It receives microphone
and tab audio as separate streams from **Verbal**, the Chrome extension in
[`dstORCH`](https://github.com/hrantsp/dstORCH), transcribes each one, and renders them
as a live conversation with the two speakers kept apart.

Kobayashi is the server: it owns the wire protocol, which is specified in
[`rec/`](rec/). Verbal does the talking; Kobayashi listens and writes it down — the
names come from *The Usual Suspects* (1995).

It accepts browser connections only from Verbal's own extension origin, and records
unencrypted audio to disk; see decision 19 in [`dstOMNI/DESIGN.md`](https://github.com/hrantsp/dstOMNI).

Built and versioned by [`dstOMNI`](https://github.com/hrantsp/dstOMNI) — see that
repository for the workspace layout and build instructions.

The protocol this serves is specified in [`rec/PROTOCOL.md`](rec/PROTOCOL.md) and checked
by `tst/abuse.mjs`, which drives a real socket with the frames a misbehaving client would
send. `python3 dstOMNI/dst.py test` runs it, the unit suite and the browser wire check
together.
