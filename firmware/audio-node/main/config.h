/**
 * config.h --- everything you are likely to change.
 *
 * Pin numbers below are the ones measured on the reference board
 * (a "WangXun V1.0" ESP32-S3 voice board). If you wire the modules
 * yourself on a breadboard, change them here and nowhere else.
 * See docs/hardware.md for the wiring table.
 */
#pragma once

/* ---------------- pins ---------------- */

/* INMP441 microphone (I2S, receive -> we only need data in) */
#define PIN_MIC_BCLK      4
#define PIN_MIC_WS        5
#define PIN_MIC_DIN       6

/* MAX98357A amplifier (I2S, transmit -> we only need data out) */
#define PIN_AMP_BCLK      16
#define PIN_AMP_LRC       17
#define PIN_AMP_DOUT      15

/* ---------------- audio format ---------------- */

/* 16 kHz / 16 bit / stereo is the cheapest format that carries speech
 * well and is what most speech tooling expects. The board stays stereo
 * on the wire even when the mic is mono: it keeps every packet frame
 * aligned to 4 bytes, which makes both sides trivial. */
#define SAMPLE_RATE       16000
#define CHUNK_FRAMES      320                  /* 20 ms @ 16 kHz */
#define CHUNK_BYTES       (CHUNK_FRAMES * 4)   /* stereo 16-bit -> 1280 B */

/* ---------------- network ---------------- */

#define UDP_PORT          5005
#define AUDIO_MAGIC       0xAA   /* first byte of every audio packet */
#define SOCK_RX_TIMEOUT_MS 5     /* keeps the receive loop responsive */

/* How much downstream audio we can queue up before we start dropping.
 * 48 KB of 16 kHz stereo is ~1.5 s. Larger = more tolerant of host
 * jitter, but also a longer delay before you hear anything. */
#define SPK_BUFFER_BYTES  (48 * 1024)

/* Safety net: if the host sends nothing at all for this long while a
 * stream is running, stop streaming (so a crashed host does not leave
 * the board shouting into the void forever).
 * 0 = disabled, which is the default: a host that only *listens* never
 * sends anything between HELLO and STOP, so any small value here would
 * cut recording sessions short. */
#define STREAM_IDLE_TIMEOUT_MS 0
