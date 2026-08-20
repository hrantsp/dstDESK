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
| Default port | `8765`, from `protocol.json`, and generated into both halves |
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
| `token` | string | Shared secret, when the server is configured with one. Omitted otherwise, which is the default. See §7. |
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
| `unauthorized-origin` | — | `Origin` header not in the allowlist. Settled during the HTTP upgrade, so there is no WebSocket to send an `error` on and no close code: the client sees the upgrade fail. Listed here because it is a rejection reason, not because it appears on the wire. |
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

Two classes of fault, treated differently. A frame that cannot be interpreted at all
means the sender and receiver disagree about the format, and nothing further on the
connection can be trusted. A frame that is well formed but arrives in the wrong place
is a transport or client fault affecting that frame alone.

**Fatal — the server MUST send `malformed-frame` and close** if any of these hold:

- The message is shorter than 12 bytes.
- `version` is not `1`.
- `stream` is outside the set defined in §5.2.
- The message length is not exactly `12 + frameSamples * 2`.
- `frameSamples` differs from the value agreed in `hello`. The handshake fixes the
  frame size for the connection; a receiver that checks it only there has not fixed
  anything, since a frame declaring any size is internally consistent.

**Non-fatal — the server MUST discard the frame and continue**, counting it, if:

- `stream` has no currently open stream. Opening and closing a stream mid-connection is
  permitted (§3), so audio arriving either side of that window is expected rather than
  exceptional.
- `sampleIndex` is not greater than the `sampleIndex` of the previous frame on that
  stream. The frame carries audio the receiver has already placed; replaying it would
  shift everything after it. One duplicate is not a reason to end a call.

The count of discarded frames MUST be reported in the session summary. A frame silently
dropped is indistinguishable from one that never existed.

### 5.4 Gaps

`sampleIndex` is authoritative for position; it is not required to be contiguous. A
jump larger than the previous frame's `frameSamples` means audio was dropped upstream.

On detecting a gap, the server MUST insert the equivalent duration of silence into
**both** the recorded stream and the stream it forwards to the transcription engine.

The second of those is the one that matters and the easier to forget. A transcription
engine times its results from the audio it has received, not from `sampleIndex`: hand it
a stream with a hole simply closed up, and every word timing it returns afterwards is
early by the length of what was lost — permanently, silently, and in exactly the
quantity that ordering across two streams depends on being right.

A server **MUST** bound the silence it will manufacture for a single gap, in both
directions — the recording and the engine stream alike. `sampleIndex` is a `u32` that
arrives from the client, and padding a gap means writing the difference; used unchecked
it is a length under the sender's control. One 1036-byte frame declaring a position four
billion samples ahead is eight gigabytes of silence, written synchronously.

The bound must exceed any gap a correct client can produce. §5.5 caps the sender's own
outbound buffer, so a genuine drop is bounded by that; anything far beyond it is a fault,
not a drop. Version 1 uses thirty seconds against a sender buffer of roughly sixteen.

On exceeding the bound the server MUST NOT pad. It MUST continue from the new position
and MUST report that this stream's timeline now has a step in it, rather than presenting
a recording that appears continuous or timings that appear aligned.

### 5.5 Backpressure

The client SHOULD bound its outbound buffer rather than let it grow without limit.

When the buffer is over its bound, the client SHOULD **discard newly captured frames
until it drains**. The alternative — trimming the oldest unsent frames so the newest
always go out — is better for a live transcript, because it drops stale audio rather
than current audio, but a `WebSocket` send queue cannot be trimmed once written to; it
requires the client to maintain its own queue and hand frames to the socket itself.
Version 1 does not, and the difference only becomes visible on a link that cannot
sustain 32 kB/s per stream, which loopback is not.

Either way `sampleIndex` makes the loss visible at the receiver, and §5.4 requires the
receiver to pad it back out, so a drop costs audio but not alignment. A client SHOULD
report the number of frames it dropped when the session ends.

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
- `sampleIndex` is a `u32`, so it wraps after 2^32 samples — about **74 hours** of
  continuous capture at 16 kHz. That figure is not a chosen limit and cannot be raised
  by editing a constant: it is the width of a field in §5.1, and changing it is a
  protocol version bump that both halves must ship together.

  Nothing needs it. The clock belongs to one `AudioContext`, which is created when
  capture starts and closed when it stops, so the 74 hours is one unbroken capture
  rather than an install lifetime. The WAV container gives out sooner anyway — RIFF
  sizes are `u32`, so a recording passes 4 GB at about 37 hours.

  Version 1 therefore does not handle the wrap. Past it every frame regresses against
  the last and is discarded under §5.3, which stops that stream recording and
  transcribing; the discarded count in the session summary is what shows it.

## 7. Security

The server listens on a loopback port, which any local process can reach.

1. **Origin — the boundary.** Chrome sends `Origin: chrome-extension://<id>` on the
   upgrade request, and page script can neither forge nor suppress it. The server MUST
   reject an upgrade whose `Origin` is present and not in its allowlist. This is what
   the security of version 1 rests on: a WebSocket to loopback needs no CORS preflight,
   so without it any page the user visits could open a socket to a running server,
   displace the live session and stream its own audio in.

   An upgrade carrying **no** `Origin` at all MUST be accepted. It cannot be a browser,
   and a native process running as this user does not need a socket to do harm; refusing
   it would only break the tools that make this protocol testable without a browser.

2. **Token — optional, and off by default.** If the server is configured with a shared
   secret, `hello.token` MUST match it and the comparison SHOULD be constant-time. There
   is no default secret. A token authenticates native clients, which the paragraph above
   argues is not the threat, and provisioning one means the same string typed in two
   places — so it is offered rather than required.

Neither check defends against a hostile process running as the same user, which could
read the config. Audio never leaves the machine except on the deliberate connection to
the transcription provider.

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
