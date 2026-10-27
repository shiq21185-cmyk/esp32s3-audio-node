#!/usr/bin/env python3
"""
Talk to yourself, with a 0.5 s delay. The fastest end-to-end smoke test:
it fails loudly if either direction is broken.

    python loopback.py 192.168.1.109 5
"""

import sys

sys.path.insert(0, "..")  # when running straight from examples/
from audio_node import AudioNode  # noqa: E402


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.109"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

    with AudioNode(host) as node:
        packets = node.loopback(seconds)

    print(f"looped {packets} packets over {seconds:.1f}s")
    return 0 if packets else 1


if __name__ == "__main__":
    raise SystemExit(main())
