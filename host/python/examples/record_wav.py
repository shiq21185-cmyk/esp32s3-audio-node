#!/usr/bin/env python3
"""Record the board's microphone to a WAV file.

    python record_wav.py 192.168.1.109 out.wav 10
"""

import sys

sys.path.insert(0, "..")  # when running straight from examples/
from audio_node import AudioNode  # noqa: E402


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.109"
    out = sys.argv[2] if len(sys.argv) > 2 else "record.wav"
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

    with AudioNode(host) as node:
        if not node.ping():
            print("board did not answer PING")
            return 1

        def meter(elapsed, peak, rms):
            bar = "#" * min(40, int(rms / 40))
            print(f"\r{elapsed:5.1f}s  peak={peak:5d}  rms={rms:7.1f} {bar:<40}",
                  end="", flush=True)

        node.record_wav(out, seconds, progress=meter)

    print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
