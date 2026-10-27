#!/usr/bin/env python3
"""
Voice-activated recording: waits for you to start talking, then records
until you stop, and writes one WAV per utterance.

This is the smallest useful thing you can build on top of the board, and
it doubles as a worked example of the API: you get raw 20 ms packets and
decide everything else yourself.

    python vad_record.py 192.168.1.109 --rounds 3 --silence 1.5

Notes from the field (they cost time to learn):
  * Measure the noise floor instead of hard-coding a threshold; a fan or
    an air conditioner moves it by 10x between rooms.
  * Throw away the first ~0.3 s after starting the stream: the first
    packets are I2S DMA garbage and will trigger any gate.
  * Require two consecutive loud packets, not one, or a door slam starts
    a recording.
  * The board is monophonic in practice (mic data is duplicated into both
    channels), so read the left channel only when measuring level.
"""

import argparse
import array
import sys
import time

sys.path.insert(0, "..")  # when running straight from examples/
from audio_node import AudioNode, pcm_to_wav  # noqa: E402

WARMUP_S = 0.3
FLOOR_S = 0.6


def level(pcm: bytes) -> tuple[int, float]:
    a = array.array("h")
    a.frombytes(pcm[: len(pcm) // 2 * 2])
    left = a[0::2]
    if not left:
        return 0, 0.0
    peak = max(max(left), -min(left))
    rms = (sum(v * v for v in left) / len(left)) ** 0.5
    return peak, rms


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--silence", type=float, default=1.5,
                    help="seconds of quiet before an utterance ends")
    ap.add_argument("--prefix", default="utterance")
    args = ap.parse_args()

    with AudioNode(args.host) as node:
        if not node.ping():
            print("no PONG")
            return 1

        for r in range(args.rounds):
            if not node.start():
                print("no READY")
                return 1

            # --- warm-up + noise floor ---
            t0 = time.time()
            warm: list[bytes] = []
            while time.time() - t0 < WARMUP_S:
                p = node.read_packet(0.3)
                if p:
                    warm.append(p)
            floors: list[float] = []
            t0 = time.time()
            while time.time() - t0 < FLOOR_S:
                p = node.read_packet(0.3)
                if p:
                    floors.append(level(p)[1])
            floors.sort()
            floor = floors[len(floors) // 5] if floors else 0.0
            gate = max(900.0, floor * 1.5)
            print(f"[round {r}] floor rms={floor:.0f} gate={gate:.0f} --- speak now")

            # --- wait for speech ---
            live_run = 0
            while True:
                p = node.read_packet(0.3)
                if p is None:
                    continue
                if level(p)[0] >= gate:
                    live_run += 1
                    if live_run >= 2:
                        break
                else:
                    live_run = 0

            # --- record until quiet ---
            frames = [p]
            quiet = 0.0
            started = time.time()
            while quiet < args.silence and time.time() - started < 30:
                p = node.read_packet(0.3)
                if p is None:
                    continue
                frames.append(p)
                if level(p)[0] < gate:
                    quiet += 0.02
                else:
                    quiet = 0.0

            node.stop()
            pcm = b"".join(frames)
            out = f"{args.prefix}-{r}.wav"
            pcm_to_wav(pcm, out)
            print(f"[round {r}] {len(pcm) / 64000:.2f}s -> {out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
