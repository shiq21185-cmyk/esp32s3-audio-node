/**
 * audio-node.mjs --- Node.js client for esp32s3-audio-node. No dependencies.
 *
 *   import { AudioNode } from "./audio-node.mjs";
 *
 *   const node = new AudioNode("192.168.1.109");
 *   await node.ping();
 *   await node.recordWav("out.wav", 5);
 *   node.on("audio", (pcm) => { /* 20 ms of s16le stereo PCM *\/ });
 *
 *   Node 18+. ES modules (`"type": "module"` or a .mjs file).
 */

import dgram from "node:dgram";
import fs from "node:fs";

export const SAMPLE_RATE = 16000;
export const CHANNELS = 2;
export const SAMPLE_WIDTH = 2;
export const FRAME_BYTES = CHANNELS * SAMPLE_WIDTH;
export const AUDIO_MAGIC = 0xaa;
export const DEFAULT_PORT = 5005;

const CHUNK_BYTES = 320 * FRAME_BYTES; // 20 ms
const PLAY_FRAMES = 160; // 10 ms

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class AudioNode {
  /**
   * @param {string} host board IP
   * @param {{port?: number, localPort?: number, timeoutMs?: number}} [opts]
   */
  constructor(host, opts = {}) {
    this.host = host;
    this.port = opts.port ?? DEFAULT_PORT;
    this.timeoutMs = opts.timeoutMs ?? 2000;
    this.streaming = false;
    this._listeners = new Map();
    this._pending = new Map(); // command -> resolve
    this._rx = [];

    this.socket = dgram.createSocket("udp4");
    this.socket.bind(opts.localPort ?? 0);
    this.socket.on("message", (msg) => this._onMessage(msg));
  }

  // ---------------------------------------------------------------- events

  on(event, fn) {
    if (!this._listeners.has(event)) this._listeners.set(event, []);
    this._listeners.get(event).push(fn);
    return this;
  }

  _emit(event, ...args) {
    for (const fn of this._listeners.get(event) ?? []) fn(...args);
  }

  _onMessage(msg) {
    if (msg[0] === AUDIO_MAGIC) {
      const pcm = msg.subarray(1);
      this._emit("audio", pcm);
      return;
    }
    const text = msg.toString("ascii").trim();
    this._emit("command", text);
    const waiter = this._pending.get(text);
    if (waiter) {
      this._pending.delete(text);
      waiter();
    }
  }

  _waitFor(token, timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._pending.delete(token);
        reject(new Error(`timeout waiting for ${token} from ${this.host}:${this.port}`));
      }, timeoutMs);
      this._pending.set(token, () => {
        clearTimeout(timer);
        resolve(true);
      });
    });
  }

  _send(text) {
    this.socket.send(Buffer.from(text, "ascii"), this.port, this.host);
  }

  // ---------------------------------------------------------------- commands

  async ping(timeoutMs = 1000) {
    const p = this._waitFor("PONG", timeoutMs).catch(() => false);
    this._send("PING");
    return p;
  }

  async start(timeoutMs = 2000) {
    const p = this._waitFor("READY", timeoutMs);
    this._send("HELLO");
    await p;
    this.streaming = true;
    return true;
  }

  stop() {
    if (this.streaming) {
      this._send("STOP");
      this.streaming = false;
    }
  }

  close() {
    try {
      this.stop();
      this.socket.close();
    } catch {
      /* already closed */
    }
  }

  // ---------------------------------------------------------------- record

  /**
   * Collect PCM for `seconds`, or until `signal` is aborted.
   * @returns {Promise<Buffer>} raw s16le stereo PCM
   */
  record(seconds, signal) {
    return new Promise(async (resolve, reject) => {
      const chunks = [];
      const onAudio = (pcm) => chunks.push(Buffer.from(pcm));
      this.on("audio", onAudio);
      const done = () => {
        this._listeners.set(
          "audio",
          (this._listeners.get("audio") ?? []).filter((f) => f !== onAudio),
        );
        this.stop();
        resolve(Buffer.concat(chunks));
      };
      try {
        await this.start();
      } catch (e) {
        this._listeners.set(
          "audio",
          (this._listeners.get("audio") ?? []).filter((f) => f !== onAudio),
        );
        return reject(e);
      }
      const timer = setTimeout(done, seconds * 1000);
      signal?.addEventListener("abort", () => {
        clearTimeout(timer);
        done();
      });
    });
  }

  async recordWav(path, seconds, signal) {
    const pcm = await this.record(seconds, signal);
    fs.writeFileSync(path, makeWav(pcm));
    return path;
  }

  // ---------------------------------------------------------------- play

  /**
   * Send raw s16le stereo PCM (16 kHz) to the speaker.
   * Paced at real time, which is what keeps the board's buffer healthy.
   */
  async playPcm(pcm, { gain = 1.0, realtime = true, padMs = 400 } = {}) {
    const pktBytes = (PLAY_FRAMES * FRAME_BYTES) | 0;
    let data = gain === 1.0 ? pcm : scaleGain(pcm, gain);

    const packets = [];
    for (let i = 0; i < data.length; i += pktBytes) {
      let chunk = data.subarray(i, i + pktBytes);
      if (chunk.length < pktBytes) {
        chunk = Buffer.concat([chunk, Buffer.alloc(pktBytes - chunk.length)]);
      }
      packets.push(Buffer.concat([Buffer.from([AUDIO_MAGIC]), chunk]));
    }

    const t0 = Date.now();
    for (let i = 0; i < packets.length; i++) {
      this.socket.send(packets[i], this.port, this.host);
      if (realtime) {
        const target = t0 + ((i + 1) * pktBytes * 1000) / (SAMPLE_RATE * FRAME_BYTES);
        const delay = target - Date.now();
        if (delay > 0) await sleep(delay);
      }
    }

    // Push the tail out of the DMA ring.
    const silence = Buffer.concat([Buffer.from([AUDIO_MAGIC]), Buffer.alloc(pktBytes)]);
    for (let i = 0; i < padMs / 10; i++) {
      this.socket.send(silence, this.port, this.host);
      if (realtime) await sleep(10);
    }
    return packets.length;
  }

  /** Play a WAV file (any bit depth, any rate, mono or stereo). */
  async playWav(path, { gain = 1.0, realtime = true } = {}) {
    const pcm = wavToPcm(fs.readFileSync(path));
    await this.playPcm(pcm, { gain, realtime });
  }
}

// ---------------------------------------------------------------- WAV helpers

/** Wrap PCM in a 44-byte WAV header. */
export function makeWav(pcm, sampleRate = SAMPLE_RATE, channels = CHANNELS) {
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(sampleRate * channels * 2, 28);
  header.writeUInt16LE(channels * 2, 32);
  header.writeUInt16LE(16, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

/**
 * WAV -> 16 kHz stereo s16le. Flattens to mono, resamples, duplicates.
 * Handles 8/16/24/32-bit PCM and float32.
 */
export function wavToPcm(buf) {
  if (buf.toString("ascii", 0, 4) !== "RIFF") throw new Error("not a RIFF/WAVE file");

  let offset = 12;
  let fmt = null;
  let data = null;
  while (offset + 8 <= buf.length) {
    const id = buf.toString("ascii", offset, offset + 4);
    const size = buf.readUInt32LE(offset + 4);
    const body = buf.subarray(offset + 8, offset + 8 + size);
    if (id === "fmt ") {
      fmt = {
        format: body.readUInt16LE(0),
        channels: body.readUInt16LE(2),
        sampleRate: body.readUInt32LE(4),
        bits: body.readUInt16LE(14),
      };
    } else if (id === "data") {
      data = body;
    }
    offset += 8 + size + (size % 2);
  }
  if (!fmt || !data) throw new Error("WAV missing fmt or data chunk");

  let samples;
  const { bits, channels, sampleRate, format } = fmt;
  if (format === 3 && bits === 32) {
    samples = new Float32Array(data.length / 4);
    for (let i = 0; i < samples.length; i++) samples[i] = data.readFloatLE(i * 4);
  } else if (bits === 8) {
    samples = new Float32Array(data.length);
    for (let i = 0; i < samples.length; i++) samples[i] = (data[i] - 128) / 128;
  } else if (bits === 16) {
    samples = new Float32Array(data.length / 2);
    for (let i = 0; i < samples.length; i++) samples[i] = data.readInt16LE(i * 2) / 32768;
  } else if (bits === 24) {
    samples = new Float32Array(data.length / 3);
    for (let i = 0; i < samples.length; i++) {
      const v = data[i * 3] | (data[i * 3 + 1] << 8) | (data[i * 3 + 2] << 16);
      samples[i] = (v & 0x800000 ? v - 0x1000000 : v) / 8388608;
    }
  } else if (bits === 32) {
    samples = new Float32Array(data.length / 4);
    for (let i = 0; i < samples.length; i++) samples[i] = data.readInt32LE(i * 4) / 2147483648;
  } else {
    throw new Error(`unsupported WAV bit depth: ${bits}`);
  }

  // left channel only
  if (channels > 1) {
    const mono = new Float32Array(Math.floor(samples.length / channels));
    for (let i = 0; i < mono.length; i++) mono[i] = samples[i * channels];
    samples = mono;
  }

  if (sampleRate !== SAMPLE_RATE) samples = resample(samples, sampleRate, SAMPLE_RATE);

  const out = Buffer.alloc(samples.length * 4);
  for (let i = 0; i < samples.length; i++) {
    const v = Math.max(-32768, Math.min(32767, Math.round(samples[i] * 32767)));
    out.writeInt16LE(v, i * 4); // left
    out.writeInt16LE(v, i * 4 + 2); // right
  }
  return out;
}

function resample(samples, src, dst) {
  const ratio = dst / src;
  const out = new Float32Array(Math.floor(samples.length * ratio));
  for (let i = 0; i < out.length; i++) {
    const pos = i / ratio;
    const i0 = Math.floor(pos);
    const frac = pos - i0;
    const s0 = i0 < samples.length ? samples[i0] : 0;
    const s1 = i0 + 1 < samples.length ? samples[i0 + 1] : 0;
    out[i] = s0 + (s1 - s0) * frac;
  }
  return out;
}

function scaleGain(pcm, gain) {
  const out = Buffer.from(pcm);
  for (let i = 0; i + 1 < out.length; i += 2) {
    const v = out.readInt16LE(i) * gain;
    out.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(v))), i);
  }
  return out;
}
