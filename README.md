# dstDESK — Kobayashi

**Kobayashi** is the cross-platform C++/Qt desktop application of the dual-stream
transcription pipeline; `dstDESK` is the repository it lives in. It receives microphone
and tab audio as separate streams from **Verbal**, the Chrome extension in
[`dstORCH`](https://github.com/hrantsp/dstORCH), transcribes each one, and renders them
as a live conversation with the two speakers kept apart.

Kobayashi is the server: it owns the wire protocol, which is specified in
[`rec/`](rec/). Verbal does the talking; Kobayashi listens and writes it down.

Built and versioned by [`dstOMNI`](https://github.com/hrantsp/dstOMNI) — see that
repository for the workspace layout and build instructions.
