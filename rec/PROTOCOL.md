# dst wire protocol — version 1

Normative specification of the link between `dstORCH` (the Chrome extension, client)
and `dstDESK` (the desktop application, server).

`dstDESK` owns this document. `dstORCH` consumes generated constants derived from it
and does not maintain its own copy. Background on why the audio parameters are what
they are is in [AUDIO-PRIMER.md](AUDIO-PRIMER.md); the architectural decisions behind
the transport are recorded in `dstOMNI/DESIGN.md`.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are used as in RFC 2119.

---

## 1. Transport

A single WebSocket connection carries everything.

| | |
|---|---|
| Endpoint | `ws://127.0.0.1:<port>/` |
| Default port | `8765`, overridden by `DST_WS_PORT` in the active target config |
| Server | `dstDESK` |
| Client | `dstORCH`, connecting from its offscreen document |

The server MUST bind to the loopback interface only. It MUST NOT bind `0.0.0.0`.

Control messages are WebSocket **text** frames containing one JSON object. Audio
travels as WebSocket **binary** frames. The frame type is the discriminator; there is
no in-band type tag at the message level.

WebSocket preserves message boundaries over TCP, so the protocol needs no length
prefixing, and frames arrive in the order they were sent.

## 2. Versioning

The protocol version is a single integer, currently **1**. It appears in the `hello`
handshake and in the header of every audio frame.

A server receiving a version it does not implement MUST reject the connection with an
`error` control message and close. It MUST NOT attempt to interpret the payload.
Silent misinterpretation of audio is the failure mode this field exists to prevent.

`dstORCH` and `dstDESK` carrying the same release tag are guaranteed to agree on this
version.

## 3. Connection lifecycle

```
client                                  server
  │                                       │
  ├─ TCP + WebSocket upgrade ────────────►│  Origin checked
  │                                       │
  ├─ {"type":"hello", ...} ──────────────►│  version + token checked
  │◄──────────────── {"type":"ready",...} ┤
  │                                       │
  ├─ {"type":"stream-open","stream":0} ──►│
  ├─ {"type":"stream-open","stream":1} ──►│
  │                                       │
  ├─ [binary audio frame] ───────────────►│   ~31 per second per stream
  ├─ [binary audio frame] ───────────────►│
  │              ⋮                        │
  ├─ {"type":"stream-close","stream":0} ─►│
  ├─ {"type":"bye"} ─────────────────────►│
  ├─ close(1000) ────────────────────────►│
```

Ordering rules:

- `hello` MUST be the first message on the connection. A server receiving anything
  else first MUST close with code `4001`.
- A `stream-open` for a given stream MUST precede any audio frame for that stream.
  Audio for an unopened stream MUST be discarded.
- A stream MAY be opened and closed repeatedly on one connection. `sampleIndex` is
  drawn from a counter shared by both streams and is never reset; it keeps advancing
  across a close and reopen.
- The client SHOULD reconnect with exponential backoff if the socket drops. The server
  MUST accept a reconnection as a fresh session with no retained state.

## 4. Control messages

Every control message is a JSON object with a `type` field. Unknown fields MUST be
ignored, so the protocol can be extended without a version bump. Unknown `type` values
MUST be ignored by the server, which MUST NOT close the connection because of one.

### 4.1 `hello` — client → server

Sent once, immediately after the socket opens.

```json
{
  "type": "hello",
  "protocol": 1,
  "token": "shared-secret-from-target-config",
  "sampleRate": 16000,
  "frameSamples": 512,
  "contextEpochUtcMs": 1755248100123.4,
  "client": "dstORCH/0.1.0"
}
```

| Field | Type | Meaning |
|---|---|---|
| `protocol` | int | Protocol version. MUST be `1`. |
| `token` | string | Shared secret from the target config. See §7. |
| `sampleRate` | int | Samples per second per stream. MUST be `16000` in version 1. |
| `frameSamples` | int | Samples per audio frame. MUST be `512` in version 1. |
| `contextEpochUtcMs` | number | UTC milliseconds corresponding to shared-clock zero. See §6. |
| `client` | string | Informational build identifier. |

### 4.2 `ready` — server → client

```json
{ "type": "ready", "protocol": 1, "server": "dstDESK/0.1.0" }
```

The client MUST NOT send audio before receiving `ready`.

### 4.3 `error` — server → client

```json
{ "type": "error", "code": "protocol-version-mismatch", "message": "server speaks 1, client offered 2" }
```

| `code` | Close code | Meaning |
|---|---|---|
| `protocol-version-mismatch` | 4001 | `hello.protocol` is not supported. |
| `unauthorized-origin` | 4002 | `Origin` header not in the allowlist. |
| `invalid-token` | 4002 | `hello.token` did not match. |
| `unsupported-audio-format` | 4001 | `sampleRate` or `frameSamples` not supported. |
| `malformed-frame` | 4003 | An audio frame failed the checks in §5.3. |
| `handshake-expected` | 4001 | First message was not `hello`. |

The server MUST send `error` before closing, so the failure is diagnosable from the
client side.

### 4.4 `stream-open` — client → server

```json
{
  "type": "stream-open",
  "stream": 0,
  "label": "microphone"
}
```

| Field | Type | Meaning |
|---|---|---|
| `stream` | int | Stream identifier. See §5.2. |
| `label` | string | Human-readable name for display. |

No timing origin is carried here. `sampleIndex` is a position on a clock shared by both
streams, so a stream needs no origin of its own. See §6.

### 4.5 `stream-close` — client → server

```json
{ "type": "stream-close", "stream": 0, "reason": "user-stopped" }
```

`reason` is informational. Suggested values: `user-stopped`, `tab-closed`,
`device-lost`, `error`.

### 4.6 `bye` — client → server

```json
{ "type": "bye" }
```

Signals an orderly shutdown. The server SHOULD flush pending transcription and close
with code `1000`.

## 5. Audio frames

### 5.1 Header layout

Binary WebSocket frame, **little-endian**, fixed 12-byte header followed by payload:

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `u8` | `version` | MUST be `1`. |
| 1 | `u8` | `stream` | Stream identifier, §5.2. |
| 2 | `u16` | `frameSamples` | Sample count in this payload. |
| 4 | `u32` | `sampleIndex` | Position of this frame's first sample on the capture clock shared by both streams. |
| 8 | `u32` | `flags` | Reserved. Senders MUST write `0`; receivers MUST ignore unknown bits. |
| 12 | `int16[]` | payload | `frameSamples` samples, signed 16-bit, little-endian. |

Total frame size is `12 + frameSamples * 2` bytes — **1036 bytes** at the version 1
defaults.

The payload begins at offset 12, which is 2-byte aligned, so a receiver MAY read it
directly as `int16_t*` without copying.

### 5.2 Stream identifiers

| Value | Stream | Source |
|---|---|---|
| `0` | microphone | What the local user says. |
| `1` | tab | What the local user hears — remote participants. |
| `2`–`255` | reserved | MUST NOT be sent in version 1. |

The two streams are transported over one connection and MUST remain separately
identified end to end. They are never mixed.

### 5.3 Receiver validation

A server MUST reject a frame as `malformed-frame` if any of the following hold:

- The message is shorter than 12 bytes.
- `version` is not `1`.
- `stream` has no corresponding open stream.
- The message length is not exactly `12 + frameSamples * 2`.
- `sampleIndex` is not greater than the `sampleIndex` of the previous frame on that
  stream.

### 5.4 Gaps

`sampleIndex` is authoritative for position; it is not required to be contiguous. A
jump larger than the previous frame's `frameSamples` means audio was dropped upstream.

On detecting a gap, the server SHOULD insert the equivalent duration of silence into
the stream it forwards to the transcription engine. Timing then stays aligned with
`sampleIndex`, and word timings returned by the engine remain usable for ordering.

### 5.5 Backpressure

The client SHOULD bound its outbound buffer. If the socket cannot keep up, it SHOULD
drop the **oldest** unsent frames rather than grow without limit; `sampleIndex` makes
the loss visible and recoverable at the receiver. Live transcription of recent audio
matters more than completeness of old audio.

## 6. Timing model

Both streams are captured through a **single `AudioContext`** in the offscreen
document. Inside an AudioWorklet, `currentFrame` is a context-global counter — the
sample index at the start of the current render quantum — shared by every node in that
context.

`sampleIndex` in the frame header is that counter. It is a position on **one timeline
belonging to both streams**, not a per-stream offset.

For any frame:

```
captureSeconds  = sampleIndex / sampleRate
utcMilliseconds = contextEpochUtcMs + captureSeconds * 1000
```

Two frames carrying the same `sampleIndex` were captured in the same render quantum.
Cross-stream ordering is therefore an integer comparison of `sampleIndex` requiring no
correction — which is what allows the two transcripts to be merged into a conversation
in natural order.

`contextEpochUtcMs` exists only so absolute wall-clock times can be shown in the UI. It
is computed once by the client as `Date.now() - audioContext.currentTime * 1000`.
Ordering never needs it.

Limits of this model:

- A shared clock removes clock **drift** between the streams. It does not remove
  differences in capture **path latency**: microphone input latency and the tab audio
  path may differ by tens of milliseconds. The protocol does not correct for this.
  Conversation ordering operates on utterances seconds apart, where such an offset is
  immaterial.
- `sampleIndex` is a `u32`, which overflows after roughly 74 hours of continuous audio
  at 16 kHz. A session reaching that duration MUST be treated as ended.

## 7. Security

The server listens on a loopback port, which any local process can reach. Version 1
applies two checks:

1. **Origin.** Chrome sends `Origin: chrome-extension://<id>` on the upgrade request.
   The server MUST reject an upgrade whose `Origin` is not in the allowlist configured
   for the active target.
2. **Token.** `hello.token` MUST match the shared secret in the target config. The
   comparison SHOULD be constant-time.

Neither check defends against a hostile process running as the same user, which could
read the config. They exist to prevent unrelated local software and stray browser tabs
from connecting by accident. Audio never leaves the machine except on the deliberate
connection to the transcription provider.

## 8. Reference constants

Generated into both codebases from this document. Hand-edited copies are a protocol
violation.

| Constant | Value |
|---|---|
| `DST_PROTOCOL_VERSION` | `1` |
| `DST_SAMPLE_RATE` | `16000` |
| `DST_FRAME_SAMPLES` | `512` |
| `DST_HEADER_BYTES` | `12` |
| `DST_FRAME_BYTES` | `1036` |
| `DST_FRAME_DURATION_MS` | `32` |
| `DST_STREAM_MIC` | `0` |
| `DST_STREAM_TAB` | `1` |
