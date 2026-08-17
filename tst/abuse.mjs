// Drives a running dstdesk with clients that break the protocol on purpose.
//
// The parsing rules are unit-tested against buffers in TestFrame.cpp, but that proves
// only that the parser rejects them — not that the server closes the connection, sends
// a diagnosable error, keeps its other state intact, and survives to serve the next
// client. Those are properties of the whole program, and only a real socket shows them.
//
//   dstDESK/bin/Release/dstdesk --headless --port 8899 --output /tmp/abuse
//   node dstDESK/tst/abuse.mjs --port 8899
//
// Exits non-zero if any case behaves other than specified in rec/PROTOCOL.md.

import { VERSION, SAMPLE_RATE, FRAME_SAMPLES, HEADER_BYTES, OFFSET, STREAM }
  from '../../dstORCH/src/generated/protocol.js';

const port = Number(
  (process.argv.find((a) => a.startsWith('--port=')) ?? '--port=8899').split('=')[1],
);

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
