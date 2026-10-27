#!/usr/bin/env python3
"""Play a WAV file on the board's speaker.

    python play_wav.py 192.168.1.109 hello.wav --gain 0.8

Any WAV the browser or ffmpeg produced will work: 8/16/24/32-bit, mono
or stereo, any sample rate.
"""

import argparse
import sys

sys.path.insert(0, "..")  # when running straight from examples/
from audio_node import AudioNode  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("wav")
    ap.add_argument("--gain", type=float, default=1.0,
                    help="1.0 = unchanged, 0.5 = half volume, >1 clips")
    ap.add_argument("--fast", action="store_true",
                    help="do not pace packets (only for clips shorter than the 1.5s buffer)")
    args = ap.parse_args()

    with AudioNode(args.host) as node:
        if not node.ping():
            print("board did not answer PING")
            return 1
        duration = node.play_wav(args.wav, gain=args.gain, realtime=not args.fast)

    print(f"played {duration:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
