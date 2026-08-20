// Session lifetime on the server, over a real socket.
//
// abuse.mjs covers what one connection may send. This covers what happens between
// connections — the part the extension actually exercises, because a reloaded extension,
// a restarted service worker or a socket blip all leave one connection alive while the
// next one handshakes. Both cases here were live faults:
//
//   1. A displaced session kept its socket wired to the server. Its frames were written
//      into the *replacement* session's recording, and its eventual disconnect tore that
//      session down. Only reachable with transcription on, because that is what makes
//      the teardown wait rather than finish inside the call that started it.
//   2. Session directories are named by the second capture began in, and two sessions
//      can share a second — the extension's first reconnect delay is 500 ms. The second
//      session's stream-open truncated the first one's mic.wav, and the summary reported
//      the frames it had written as a clean session.
//
// Needs a server started with recording ON. dst.py test starts one; by hand:
//
//   kobayashi-mockstt --port 9001 &
//   DEEPGRAM_API_KEY=mock kobayashi --headless --port 8899 --output /tmp/sessions \
//       --stt-endpoint ws://127.0.0.1:9001 &
//   node dstDESK/tst/sessions.mjs --port 8899 --output /tmp/sessions
import fs from 'node:fs';
import path from 'node:path';
import { VERSION, SAMPLE_RATE, FRAME_SAMPLES, HEADER_BYTES, OFFSET, STREAM }
  from '../../dstORCH/src/generated/protocol.js';

function argOf(argv, name, fallback) {
  const at = argv.indexOf(`--${name}`);
  if (at >= 0 && argv[at + 1] !== undefined) return argv[at + 1];
  const inline = argv.find((a) => a.startsWith(`--${name}=`));
  return inline !== undefined ? inline.slice(name.length + 3) : fallback;
}

const argv   = process.argv.slice(2);
const port   = Number(argOf(argv, 'port', 8899));
const output = argOf(argv, 'output', null);

if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error(`Invalid --port: ${port}`);
  process.exit(2);
}

let failures = 0;
function check(label, ok, detail = '') {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? ` — ${detail}` : ''}`);
  if (!ok) ++failures;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function frame(stream, sampleIndex, fill) {
  const buffer = new ArrayBuffer(HEADER_BYTES + FRAME_SAMPLES * 2);
  const view = new DataView(buffer);
  view.setUint8(OFFSET.version, VERSION);
  view.setUint8(OFFSET.stream, stream);
  view.setUint16(OFFSET.frameSamples, FRAME_SAMPLES, true);
  view.setUint32(OFFSET.sampleIndex, sampleIndex >>> 0, true);
  view.setUint32(OFFSET.flags, 0, true);
  for (let ii = 0; ii < FRAME_SAMPLES; ++ii) view.setInt16(HEADER_BYTES + ii * 2, fill, true);
  return buffer;
}

function hello(client) {
  return JSON.stringify({ type: 'hello', protocol: VERSION, sampleRate: SAMPLE_RATE,
                          frameSamples: FRAME_SAMPLES, client,
                          contextEpochUtcMs: Date.now() });
}

/** Opens a socket, handshakes, and opens the microphone stream. */
async function session(client) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  ws.binaryType = 'arraybuffer';

  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('connect timed out')), 5000);
    ws.addEventListener('open', () => { clearTimeout(timer); resolve(); });
    ws.addEventListener('error', () => { clearTimeout(timer); reject(new Error('connect failed')); });
  });

  const ready = new Promise((resolve) => {
    const timer = setTimeout(() => resolve(false), 5000);
    ws.addEventListener('message', (event) => {
      if (JSON.parse(event.data).type === 'ready') { clearTimeout(timer); resolve(true); }
    });
  });

  ws.send(hello(client));
  if (!(await ready)) throw new Error(`${client}: server never said ready`);
  ws.send(JSON.stringify({ type: 'stream-open', stream: STREAM.MIC, label: 'microphone' }));
  return ws;
}

function burst(ws, count, from, fill) {
  for (let ii = 0; ii < count; ++ii)
    ws.send(frame(STREAM.MIC, from + ii * FRAME_SAMPLES, fill));
}

/** Every 16-bit sample value present in a WAV's data chunk. */
function valuesIn(file) {
  const data = fs.readFileSync(file).subarray(44);
  const seen = new Set();
  for (let at = 0; at + 1 < data.length; at += 2) seen.add(data.readInt16LE(at));
  return seen;
}

function directoriesIn(base) {
  return fs.readdirSync(base, { withFileTypes: true })
           .filter((entry) => entry.isDirectory())
           .map((entry) => entry.name)
           .sort();
}

console.log(`Session lifetime against ws://127.0.0.1:${port}\n`);

// ── 1. a displaced session must let go of its socket ─────────────────────────
console.log('Displacement:');
{
  const before = output ? directoriesIn(output) : [];

  const A = await session('sessions.mjs/displaced');
  burst(A, 64, SAMPLE_RATE, 0x1111);
  await sleep(1200);                      // long enough for its engine link to come up

  const B = await session('sessions.mjs/live');
  burst(B, 16, SAMPLE_RATE, 0x2222);
  await sleep(400);

  // A is displaced but still connected. Its audio sits ahead of B's clock, so a recorder
  // that accepts it will write it rather than reject it as going backwards.
  burst(A, 16, SAMPLE_RATE + 100000 * FRAME_SAMPLES, 0x7777);
  await sleep(600);

  // And when it goes away, it must take nothing with it.
  A.close();
  await sleep(1200);

  check('the live session survives a displaced client disconnecting',
        B.readyState === WebSocket.OPEN, `readyState=${B.readyState}`);

  if (B.readyState === WebSocket.OPEN) burst(B, 16, SAMPLE_RATE + 200000 * FRAME_SAMPLES, 0x2222);
  await sleep(400);
  B.send(JSON.stringify({ type: 'bye' }));
  await sleep(800);
  try { B.close(); } catch { /* already gone */ }
  await sleep(400);

  if (output) {
    const added = directoriesIn(output).filter((name) => !before.includes(name));
    const files = added.map((name) => path.join(output, name, 'mic.wav')).filter(fs.existsSync);
    const live  = files.at(-1);

    check('the displaced client wrote nothing into the live recording',
          live !== undefined && !valuesIn(live).has(0x7777),
          live === undefined ? 'no recording found'
                             : `${path.basename(path.dirname(live))}/mic.wav`);
  }
}

// ── 2. two sessions inside one second must not share a directory ─────────────
console.log('\nDirectories:');
if (!output) {
  console.log('  skipped — no --output given');
} else {
  const before = directoriesIn(output);

  // Lined up with the start of a wall-clock second so both land inside it.
  await sleep(1000 - (Date.now() % 1000) + 20);
  const started = Math.floor(Date.now() / 1000);

  for (const [client, fill] of [['sessions.mjs/first', 0x3333], ['sessions.mjs/second', 0x4444]]) {
    const ws = await session(client);
    burst(ws, 32, SAMPLE_RATE, fill);
    ws.send(JSON.stringify({ type: 'bye' }));
    await sleep(60);
    try { ws.close(); } catch { /* already gone */ }
    await sleep(60);
  }
  const sameSecond = Math.floor(Date.now() / 1000) === started;
  await sleep(600);

  const added = directoriesIn(output).filter((name) => !before.includes(name));

  if (!sameSecond) {
    console.log('  skipped — the two sessions straddled a second boundary');
  } else {
    check('two sessions in one second get a directory each', added.length === 2,
          `created ${added.length}: ${added.join(', ')}`);

    const kept = added
      .map((name) => path.join(output, name, 'mic.wav'))
      .filter(fs.existsSync)
      .flatMap((file) => [...valuesIn(file)]);

    check("neither session's recording was overwritten by the other",
          kept.includes(0x3333) && kept.includes(0x4444),
          `values on disk: ${[...new Set(kept)].map((vv) => `0x${vv.toString(16)}`).join(', ')}`);
  }
}

console.log(failures === 0
  ? '\nSession lifetime behaved as specified.'
  : `\n${failures} case(s) did not behave as specified.`);
process.exit(failures === 0 ? 0 : 1);
