#!/usr/bin/env python3
"""Decode, synthesise and replay PLOAM frames for the zx279133 bench.

Three things this exists for:

  decode  turn a capture (the output of `cat /sys/class/net/eth0/pon_ploam`)
          into something readable, and tell you which message ids a real OLT
          actually sends before the fibre is pulled.
  synth   write a synthetic downstream script that walks the state machine
          O2-3 -> O4 -> O5 with no fibre at all.  The kernel re-seals the MIC
          for injected frames, so the 4 MIC bytes here can be anything.
  replay  print (or run) the zxponctl commands that feed a capture back in.

Frame layout, straight from zxpon_rx_ploam(): frame[0..1] ONU-ID big endian,
frame[2] message id, frame[4..] content, last 4 bytes MIC.
"""

import argparse
import sys

LEN = 48

DS_NAMES = {
    0x01: "Profile (Upstream_Overhead)",
    0x02: "Serial_Number_ONU",
    0x03: "Assign_ONU-ID",
    0x04: "Ranging_Time",
    0x05: "Deactivate_ONU-ID",
    0x06: "Disable_Serial_Number",
    0x07: "Encapsulated_OMCI(alt)",
    0x09: "Request_Registration",
    0x0A: "Assign_Alloc-ID",
    0x0B: "Key_Control(alt)",
    0x0D: "Key_Control",
}

US_NAMES = {
    0x01: "Serial_Number_ONU",
    0x02: "Registration",
    0x05: "Key_Report",
    0x09: "Acknowledge",
}

BCAST = 0x03FF


def name_for(direction, mid):
    table = US_NAMES if direction == "T" else DS_NAMES
    return table.get(mid, "unknown")


def frame(onu, mid, payload=b""):
    body = payload.ljust(LEN - 4, b"\x00")
    f = bytes([(onu >> 8) & 0xFF, onu & 0xFF, mid]) + b"\x00"
    return f + body[: LEN - 4 - 4] + b"\x00\x00\x00\x00"


def cmd_decode(args):
    seen = {}
    for line in open(args.path, "r", encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("rx=") or " " not in line:
            continue
        direction, hexs = line.split(None, 1)
        hexs = hexs.strip()
        if len(hexs) < 2 * LEN:
            continue
        raw = bytes.fromhex(hexs[: 2 * LEN])
        onu = (raw[0] << 8) | raw[1]
        mid = raw[2]
        key = (direction, onu, mid)
        seen[key] = seen.get(key, 0) + 1
        if args.quiet and seen[key] > 1:
            continue
        body = raw[4:LEN - 4]
        ascii_body = "".join(chr(c) if 32 <= c < 127 else "." for c in body)
        print(f"{direction} onu={onu:#06x} id={mid:#04x} {name_for(direction, mid)}"
              f"  body={body.hex()}")
        print(f"    ascii  {ascii_body}")
        if direction == "R" and mid == 0x01 and len(body) >= 28:
            # G.987.3 11.3.3.1 Profile: body[0] VVVV00PP, body[1] FEC,
            # body[2] delimiter len, body[11] preamble len,
            # body[12] preamble repeat, body[21:29] PON-TAG.
            print(f"    profile idx={body[0] & 3} ver={body[0] >> 4} "
                  f"fec={body[1] & 1} delim_len={body[2] & 0x0f} "
                  f"pre_len={body[11] & 0x0f} pre_repeat={body[12] & 0x1f} "
                  f"pon_tag={body[21:29].hex()}")
        if direction == "R" and mid == 0x04 and len(body) >= 5:
            eqd = int.from_bytes(body[1:5], "big")
            print(f"    ranging eqd={eqd} ({eqd / 2.48832:.1f} us) "
                  f"absolute={bool(body[0] & 1)}")
    print()
    print("summary:")
    for (direction, onu, mid), n in sorted(seen.items()):
        print(f"  {direction} onu={onu:#06x} id={mid:#04x} "
              f"{name_for(direction, mid):24s} x{n}")


def cmd_synth(args):
    onu_id = args.onu_id
    lines = [
        ("Upstream_Overhead (broadcast, as a real OLT sends it)",
         frame(BCAST, 0x01, bytes([0x00] * 4))),
        ("Assign_ONU-ID", frame(BCAST, 0x03, bytes([(onu_id >> 8) & 0xFF,
                                                    onu_id & 0xFF]))),
        ("Ranging_Time", frame(onu_id, 0x04, bytes([0x00, 0x00, 0x00, 0x00]))),
        ("Request_Registration", frame(onu_id, 0x09, b"")),
        ("Assign_Alloc-ID (alloc 1024, OMCC flag)", frame(
            onu_id, 0x0A, bytes([0x04, 0x00, 0x01, 0x00, 0x00]))),
        ("Key_Control (request new key)", frame(onu_id, 0x0D, bytes([0x00]))),
    ]
    for note, f in lines:
        print(f"# {note}")
        print(f"zxponctl ploam {f.hex()}")
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            for note, f in lines:
                fh.write(f"# {note}\n")
                fh.write(f"{f.hex()}\n")
        print(f"\n# written to {args.out}", file=sys.stderr)


def cmd_replay(args):
    onu_id = args.onu_id
    cmds = [
        "echo force > /sys/class/net/eth0/pon_arm",
        f"zxponctl identity {args.sn} {args.regid}",
    ]
    for line in open(args.path, "r", encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("rx="):
            continue
        if " " in line:
            line = line.split(None, 1)[1].strip()
        if len(line) < 2 * LEN:
            continue
        cmds.append(f"zxponctl ploam {line[: 2 * LEN]}")
        cmds.append(f"echo \"  -> $(cat /sys/class/net/eth0/pon_state)\"")
    cmds.append("cat /sys/class/net/eth0/pon_state")
    print("\n".join(cmds))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("decode", help="decode a pon_ploam capture")
    d.add_argument("path")
    d.add_argument("-q", "--quiet", action="store_true",
                   help="print each distinct frame once")
    d.set_defaults(func=cmd_decode)

    s = sub.add_parser("synth", help="emit a synthetic O2-3 -> O5 script")
    s.add_argument("--onu-id", type=int, default=7)
    s.add_argument("--out")
    s.set_defaults(func=cmd_synth)

    r = sub.add_parser("replay", help="emit zxponctl commands for a capture")
    r.add_argument("path")
    r.add_argument("--sn", default="CMDCA133E348")
    r.add_argument("--regid", default="8852308006")
    r.add_argument("--onu-id", type=int, default=7)
    r.set_defaults(func=cmd_replay)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
