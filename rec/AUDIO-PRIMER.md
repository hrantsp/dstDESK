# Audio primer

Background for [PROTOCOL.md](PROTOCOL.md). Nothing here is normative — it explains why
the numbers in the specification are the numbers they are.

## What is being transported

Sound is air pressure varying over time. A microphone measures that pressure and
reports a number. Do that 16,000 times per second and store each measurement as a
signed 16-bit integer, and the result is PCM: **an array of `int16`, and nothing
else**. No compression, no container, no framing. In C++ the entire data model is
`int16_t[]`.

Two figures follow directly:

```
16,000 samples/second × 2 bytes = 32,000 bytes per second, per stream
```

Everything else in the protocol is bookkeeping around that number.

## Why 16 kHz

A sampled signal can only represent frequencies up to half its sample rate — the
Nyquist limit. At 16 kHz that ceiling is 8 kHz, which covers essentially all of what
makes speech intelligible; telephony managed with a 4 kHz band. Music is sampled at
44.1 or 48 kHz because instruments carry energy up to about 20 kHz, but none of that
helps a speech model.

Speech recognition models are trained at 16 kHz. Sending 48 kHz audio would triple the
bandwidth and then be resampled back down at the far end.

## Why signed 16-bit

16 bits gives roughly 96 dB of dynamic range, comfortably beyond what a meeting needs.
8-bit audio is audibly noisy; 24- and 32-bit are for production audio work where
repeated processing accumulates rounding error. Neither applies here.

## Why uncompressed

Opus would cut the bandwidth by roughly 10×. It was rejected because:

- 32 kB/s per stream is free on a loopback interface that moves gigabytes per second.
- It would add a codec dependency to both codebases.
- It would destroy the property that makes the desktop side simple — see below.

## The no-transcoding property

16 kHz, mono, signed 16-bit little-endian PCM is exactly Deepgram's `linear16`
encoding. Bytes produced by the browser's AudioWorklet cross the socket and enter the
transcription connection **untouched**.

There is no resampling, no channel mixing, and no format conversion anywhere in the
C++ application. It routes bytes and draws a UI. That is a substantially smaller thing
to get right than an audio processing pipeline, and it eliminates an entire category of
bug where audio is subtly corrupted in a way that only shows up as poor transcription
accuracy.

## What one frame costs

512 samples is 32 milliseconds of sound — shorter than a single consonant, and
unrecognisable on its own.

| | |
|---|---|
| Payload | 512 × 2 = 1024 bytes |
| Header | 12 bytes |
| WebSocket framing | 8 bytes (2 header + 2 extended length + 4 client mask) |
| **On the wire** | **1044 bytes** |

At 16 kHz, a stream produces 31.25 such frames per second.

## What a sentence costs

"Hi there." takes about one second to say.

| | |
|---|---|
| Samples | 16,000 |
| Frames sent | ~31 |
| Bytes transferred | ~32 KB |
| Resulting transcript | `"Hi there."` — 9 bytes |

Roughly **3,600× more data moves than meaning comes out**. That ratio is why streaming
transcription is a metered service, and why the transcript — not the audio — is the
thing worth retaining.

## What a meeting costs

```
32,000 B/s × 3,600 s × 2 streams = 230 MB per hour
```

Trivial to move across loopback, and about 512 kbit/s in total leaving the machine for
the transcription provider.

The operational consequence: **raw audio is never accumulated.** Frames are forwarded
and released. Only transcript text, a few kilobytes per hour, is retained.

## Why 512 samples per frame

An AudioWorklet processes audio in fixed quanta of 128 samples. 512 is exactly four of
them, so no partial-block bookkeeping is needed on the browser side.

The latency contribution is 32 ms. Streaming transcription takes a few hundred
milliseconds to return interim results, so frame size accounts for well under 10% of
end-to-end delay. Halving it would not be perceptible; the tidy relationship with the
worklet quantum is worth more than the milliseconds.

## Why one AudioContext

Both captures — microphone and tab — are fed into a single `AudioContext`, so both
streams are timestamped against one monotonic clock.

The alternative, two independent capture paths, means two clocks that drift relative to
each other. Every cross-stream ordering question then requires estimating that drift,
and the "conversation in natural order" requirement becomes an approximation rather
than arithmetic.

Inside an AudioWorklet, `currentFrame` is a counter belonging to the *context*, not to
the node reading it — the sample index at the start of the current 128-sample block.
Both capture taps read the same counter in the same render pass, so equal `sampleIndex`
values mean "captured at the same instant" by construction rather than by measurement.

This is the one audio decision that is genuinely expensive to reverse later, because
the transcript merge logic is built on top of it.

## Capturing a tab silences it

`chrome.tabCapture` mutes the tab it captures. The audio has to be played back
deliberately or the user simply stops hearing the meeting.

The tempting fix — routing playback through the capture `AudioContext` — is wrong. That
context runs at 16 kHz for the transcription engine's benefit, and pushing the meeting
through it would band-limit what the user hears to 8 kHz for the whole call. The
transcript is not worth degrading the conversation it is transcribing.

Playback therefore runs on its own path at the native rate, outside the capture graph
entirely.
