#!/usr/bin/env python3
"""Minimal FT8 sidecar for WSJT-X (protocol v1, doc/ft8-sidecar-protocol.md).

It doesn't decode anything: it reports one fake decode per final pass,
which is enough to check the wiring end to end.

    WSJTX_FT8_DECODER="python3 /path/to/ft8_sidecar_example.py" wsjtx
"""
import array
import json
import sys


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main():
    stdin = sys.stdin.buffer
    while True:
        line = stdin.readline()
        if not line:
            return                          # WSJT-X closed stdin
        req = json.loads(line)
        kind = req.get("type")
        if kind == "hello":
            print(f"hello from {req.get('app')} {req.get('version')}", file=sys.stderr)
        elif kind == "quit":
            return
        elif kind == "decode":
            raw = stdin.read(2 * req["samples"])
            audio = array.array("h")
            audio.frombytes(raw)
            if sys.byteorder != "little":
                audio.byteswap()
            if req["final"]:
                peak = max((abs(s) for s in audio), default=0)
                p = req["params"]
                send({"seq": req["seq"], "snr": -10, "dt": 0.1, "freq": p["nfqso"],
                      "msg": f"CQ TEST {p['mygrid'][:4] or 'AA00'}"})
                send({"log": f"seq {req['seq']}: {len(audio)} samples, peak {peak}"})
            send({"seq": req["seq"], "done": True})


if __name__ == "__main__":
    main()
