# dstDESK

Cross-platform C++/Qt desktop application for the dual-stream transcription pipeline.
It receives microphone and tab audio as separate streams from the
[`dstORCH`](https://github.com/hrantsp/dstORCH) Chrome extension, transcribes each one,
and renders them as a live conversation with the two speakers kept apart.

`dstDESK` is the server: it owns the wire protocol, which is specified in
[`protocol/`](protocol/).

Built and versioned by [`dstOMNI`](https://github.com/hrantsp/dstOMNI) — see that
repository for the workspace layout and build instructions.
