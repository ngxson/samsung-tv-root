#!/usr/bin/env python3
"""Save every voice session from the receiver as a WAV file.

Hold MIC on the remote, speak, release: one WAV per press in voice-out/.

Usage:
  python3 voice_capture.py            follow logs/latest.log (run ./monitor.sh too)
  python3 voice_capture.py LOG --once decode the sessions already in LOG and exit

Reads the "voice: FRAME" lines that monitor.sh logs, so it doesn't need the
serial port. Each audio report (0xF6, 172 bytes) is a sequence byte followed
by three 57-byte mSBC frames (sync AD 00 00): 16 kHz mono, 22.5 ms per
report (REMOTE_PROTO.md §6.4). ffmpeg decodes the mSBC.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FRAME_RE = re.compile(r"voice: FRAME \d+ len=(\d+) ([0-9a-f]+)")
MSBC_SYNC = b"\xad\x00\x00"
MSBC_FRAME = 57


class Session:
    def __init__(self):
        self.audio = bytearray()
        self.reports = 0
        self.lost = 0
        self.bad = 0
        self.seq = None

    def add(self, report):
        if len(report) != 1 + 3 * MSBC_FRAME or any(
                report[1 + i * MSBC_FRAME:4 + i * MSBC_FRAME] != MSBC_SYNC for i in range(3)):
            self.bad += 1
            return
        if self.seq is not None:
            self.lost += (report[0] - self.seq - 1) % 256
        self.seq = report[0]
        self.reports += 1
        self.audio += report[1:]


def save(session, out_dir):
    if not session.reports:
        return
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, time.strftime("voice-%Y%m%d-%H%M%S.wav"))
    n = 1
    while os.path.exists(path):
        path = os.path.join(out_dir, time.strftime(f"voice-%Y%m%d-%H%M%S-{n}.wav"))
        n += 1
    try:
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "sbc", "-i", "pipe:0",
                        "-ar", "16000", path], input=bytes(session.audio), check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"ffmpeg failed ({e}); is ffmpeg installed?", file=sys.stderr)
        return
    notes = []
    if session.lost:
        notes.append(f"{session.lost} reports lost")
    if session.bad:
        notes.append(f"{session.bad} non-mSBC reports skipped")
    print(f"saved {os.path.relpath(path)}: {session.reports * 0.0225:.1f} s, {session.reports} reports"
          + (f" ({', '.join(notes)})" if notes else ""), flush=True)


def handle(line, state, out_dir):
    """Feed one log line; state holds the current Session or None"""
    if "voice: SESSION START" in line:
        save(state.pop("s", None) or Session(), out_dir)
        state["s"] = Session()
    elif "voice: SESSION END" in line:
        save(state.pop("s", None) or Session(), out_dir)
    else:
        m = FRAME_RE.search(line)
        if m:
            state.setdefault("s", Session()).add(bytes.fromhex(m.group(2)))


def follow(log, out_dir):
    state = {}
    print(f"following {log}; hold MIC on the remote, speak, release. Ctrl+C to quit.", flush=True)
    path = f = None
    buf = ""
    while True:
        real = os.path.realpath(log)
        if real != path:
            if f:
                f.close()
            try:
                f = open(real, errors="replace")
            except OSError:
                f = None
                time.sleep(0.5)
                continue
            if path is None:
                f.seek(0, os.SEEK_END)  # only new sessions at startup
            path = real
            buf = ""
        chunk = f.read()
        if not chunk:
            time.sleep(0.1)
            continue
        buf += chunk
        *lines, buf = buf.split("\n")
        for line in lines:
            handle(line, state, out_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default=os.path.join(HERE, "logs", "latest.log"))
    ap.add_argument("--once", action="store_true", help="decode the sessions already in LOG and exit")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "voice-out"))
    args = ap.parse_args()
    try:
        if args.once:
            state = {}
            with open(args.log, errors="replace") as f:
                for line in f:
                    handle(line, state, args.out)
            save(state.pop("s", None) or Session(), args.out)
        else:
            follow(args.log, args.out)
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
