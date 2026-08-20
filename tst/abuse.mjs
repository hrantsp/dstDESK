// Conformance checks for the server side of rec/PROTOCOL.md.
//
// The unit suite tests the pieces; this tests the *contract* — that a malformed frame
// is refused with the right code, that a merely misplaced one is discarded without
// killing the connection, and that the server is still usable after all of it. It talks
// to a real socket, so it exercises the handshake, the close codes and the framing
// exactly as the extension would.
//
// It is the only thing that checks the MUSTs in §5.3 are actually implemented, so it
// belongs in the build rather than in someone's memory: `dst.py test` starts a server
// and runs it. To run it by hand against a server you already have:
//
//   dstDESK/bin/Release/kobayashi --headless --no-transcribe --port 8899 --output /tmp/abuse
//   node dstDESK/tst/abuse.mjs --port 8899
//
// Exits non-zero if any case did not behave as the specification says.
import { VERSION, SAMPLE_RATE, FRAME_SAMPLES, HEADER_BYTES, OFFSET, STREAM }
  from '../../dstORCH/src/generated/protocol.js';

// Both `--port 8899` and `--port=8899`. Accepting only one form and reading the other
// as NaN is how a wrong port turns into "the server refused everything" — which looks
// exactly like the failure this file exists to detect.
function portFrom(argv) {
  const at = argv.indexOf('--port');
  if (at >= 0 && argv[at + 1] !== undefined) return Number(argv[at + 1]);

  const inline = argv.find((a) => a.startsWith('--port='));
  if (inline !== undefined) return Number(inline.split('=')[1]);

  return 8899;
}

const port = portFrom(process.argv.slice(2));
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error(`Invalid --port: ${port}`);
  process.exit(2);
}

let failures = 0;

function check(label, ok, detail = '') {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? ` — ${detail}` : ''}`);
  if (!ok) ++failures;
}

function open() {
  const socket = new WebSocket(`ws://127.0.0.1:${port}`);
  socket.binaryType = 'arraybuffer';
  return socket;
}

/** Resolves with { code, closeCode } once the server has had its say. */
function converse(steps, { expectClose = true, timeout = 6000 } = {}) {
  return new Promise((resolve) => {
    const socket = open();
    const result = { code: null, closeCode: null, ready: false, messages: 0 };

    const timer = setTimeout(() => {
      result.timedOut = true;
      try { socket.close(); } catch { /* already gone */ }
      resolve(result);
    }, timeout);

    socket.addEventListener('open', () => steps(socket));

    socket.addEventListener('message', (event) => {
      ++result.messages;
      const message = JSON.parse(event.data);
      if (message.type === 'error') result.code = message.code;
      if (message.type === 'ready') result.ready = true;
      if (!expectClose && result.messages >= 1) {
        clearTimeout(timer);
        socket.close();
        resolve(result);
      }
    });

    socket.addEventListener('close', (event) => {
      clearTimeout(timer);
      result.closeCode = event.code;
      resolve(result);
    });
  });
}

function hello(overrides = {}) {
  return JSON.stringify({
    type: 'hello',
    protocol: VERSION,
    sampleRate: SAMPLE_RATE,
    frameSamples: FRAME_SAMPLES,
    client: 'abuse.mjs',
    ...overrides,
  });
}

function frame({ version = VERSION, stream = STREAM.MIC, samples = FRAME_SAMPLES,
                 declared = null, sampleIndex = SAMPLE_RATE } = {}) {
  const buffer = new ArrayBuffer(HEADER_BYTES + samples * 2);
  const view = new DataView(buffer);
  view.setUint8(OFFSET.version, version);
  view.setUint8(OFFSET.stream, stream);
  view.setUint16(OFFSET.frameSamples, declared ?? samples, true);
  view.setUint32(OFFSET.sampleIndex, sampleIndex >>> 0, true);
  view.setUint32(OFFSET.flags, 0, true);
  return buffer;
}

// ── the cases ────────────────────────────────────────────────────────────────

console.log(`Abusing ws://127.0.0.1:${port}\n`);
console.log('Handshake:');

{
  const r = await converse((ws) => ws.send(JSON.stringify({ type: 'bye' })));
  check('a first message that is not hello is refused',
        r.code === 'handshake-expected', `code=${r.code} close=${r.closeCode}`);
}

{
  const r = await converse((ws) => ws.send(hello({ protocol: VERSION + 1 })));
  check('a foreign protocol version is refused',
        r.code === 'protocol-version-mismatch', `code=${r.code}`);
}

{
  const r = await converse((ws) => ws.send(hello({ sampleRate: 48000 })));
  check('an unsupported sample rate is refused',
        r.code === 'unsupported-audio-format', `code=${r.code}`);
}

{
  const r = await converse((ws) => ws.send(hello({ frameSamples: 1024 })));
  check('an unsupported frame size is refused',
        r.code === 'unsupported-audio-format', `code=${r.code}`);
}

{
  // PROTOCOL.md §4: unknown control types are ignored rather than fatal, so the
  // protocol can gain messages without a version bump.
  const r = await converse((ws) => {
    ws.send(hello());
    ws.send(JSON.stringify({ type: 'something-from-the-future', payload: 1 }));
  }, { expectClose: false });
  check('an unknown control type is ignored, not fatal', r.ready === true,
        `ready=${r.ready} code=${r.code}`);
}

{
  const r = await converse((ws) => {
    ws.send(hello());
    ws.send('{ this is not json');
  }, { expectClose: false });
  check('unparseable JSON is ignored, not fatal', r.ready === true,
        `ready=${r.ready} code=${r.code}`);
}

console.log('\nAudio frames:');

{
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => ws.send(frame({ version: VERSION + 1 })), 150);
  });
  check('a frame with a foreign version is refused',
        r.code === 'malformed-frame', `code=${r.code}`);
}

{
  const r = await converse((ws) => {
    ws.send(hello());
    // Declares more samples than the payload carries: the shape of a truncated frame.
    setTimeout(() => ws.send(frame({ samples: 64, declared: 512 })), 150);
  });
  check('a frame whose declared length disagrees is refused',
        r.code === 'malformed-frame', `code=${r.code}`);
}

{
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => ws.send(frame({ stream: 7 })), 150);
  });
  check('a frame for an unknown stream is refused',
        r.code === 'malformed-frame', `code=${r.code}`);
}

{
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => ws.send(new ArrayBuffer(4)), 150); // shorter than a header
  });
  check('a frame shorter than the header is refused',
        r.code === 'malformed-frame', `code=${r.code}`);
}

{
  // PROTOCOL.md §3: audio for a stream that was never opened is discarded, not fatal.
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => {
      ws.send(frame({ stream: STREAM.TAB }));
      ws.send(JSON.stringify({ type: 'bye' }));
    }, 150);
  }, { expectClose: true });
  check('audio for an unopened stream is discarded, not fatal',
        r.code === null, `code=${r.code}`);
}

{
  // PROTOCOL.md §5.3: hello fixes the frame size for the connection, so a later frame
  // declaring a different one is fatal. Checking it only in the handshake fixes
  // nothing — a frame of any size is internally consistent, and this one is 128 times
  // the negotiated size.
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => {
      ws.send(JSON.stringify({ type: 'stream-open', stream: STREAM.MIC, label: 'Microphone' }));
      ws.send(frame({ stream: STREAM.MIC, samples: 65535 }));
    }, 150);
  });
  check('a frame larger than the negotiated size is refused',
        r.code === 'malformed-frame', `code=${r.code}`);
}

{
  // PROTOCOL.md §5.3: a frame that goes backwards on the shared clock carries audio
  // already placed. It is discarded and counted, and the connection survives — one
  // duplicate is not a reason to end a call.
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => {
      ws.send(JSON.stringify({ type: 'stream-open', stream: STREAM.MIC, label: 'Microphone' }));
      ws.send(frame({ stream: STREAM.MIC, sampleIndex: SAMPLE_RATE * 2 }));
      ws.send(frame({ stream: STREAM.MIC, sampleIndex: SAMPLE_RATE }));
      ws.send(JSON.stringify({ type: 'bye' }));
    }, 150);
  });
  check('a frame that goes backwards is discarded, not fatal',
        r.code === null, `code=${r.code}`);
}

{
  // PROTOCOL.md §3: a stream may be closed and opened again on one connection. The
  // second open used to reuse the first one's filename and truncate it, and report the
  // loss as a clean session.
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => {
      const openMic = JSON.stringify({ type: 'stream-open', stream: STREAM.MIC, label: 'Microphone' });
      ws.send(openMic);
      ws.send(frame({ stream: STREAM.MIC, sampleIndex: SAMPLE_RATE }));
      ws.send(JSON.stringify({ type: 'stream-close', stream: STREAM.MIC, reason: 'probe' }));
      ws.send(openMic);
      ws.send(frame({ stream: STREAM.MIC, sampleIndex: SAMPLE_RATE * 3 }));
      ws.send(JSON.stringify({ type: 'bye' }));
    }, 150);
  });
  check('a stream may be closed and opened again', r.code === null, `code=${r.code}`);
}

console.log('\nSurvival:');

{
  // The point of every case above: after all that, the server is still usable. A
  // process that rejects bad input and then stops serving has not really rejected it.
  const r = await converse((ws) => {
    ws.send(hello());
    setTimeout(() => {
      ws.send(JSON.stringify({ type: 'stream-open', stream: STREAM.MIC, label: 'Microphone' }));
      ws.send(frame({ stream: STREAM.MIC }));
      ws.send(JSON.stringify({ type: 'stream-close', stream: STREAM.MIC, reason: 'done' }));
      ws.send(JSON.stringify({ type: 'bye' }));
    }, 150);
  });
  check('a well-formed session still works afterwards', r.ready === true && r.code === null,
        `ready=${r.ready} code=${r.code}`);
}

console.log(failures === 0
  ? '\nAll cases behaved as specified.'
  : `\n${failures} case(s) did not behave as specified.`);
process.exit(failures === 0 ? 0 : 1);
