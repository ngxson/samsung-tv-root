#!/usr/bin/env python3
"""Validate Flipper audio on non-macOS hosts; see README for the macOS limitation."""

import json
import sys
import time


def main():
    # sounddevice initializes PortAudio at import time. Device selection happens
    # later, so even a name-checked stream cannot isolate initialization from
    # other inputs. The macOS run was reported to activate the user's AirPods.
    # Refuse before importing the audio backend or touching the Flipper.
    if sys.platform == "darwin":
        raise SystemExit(
            "Audio capture validation is disabled on macOS: the PortAudio path "
            "affected the default AirPods input despite selecting Flipper. "
            "Use tools/micctl.py status for USB-only diagnostics, or run the "
            "audio check on a Linux host."
        )

    import numpy as np
    import sounddevice as sd

    from micctl import find_port, request

    devices = [
        (i, d)
        for i, d in enumerate(sd.query_devices())
        if d["name"] == "Flipper Dummy Mic" and d["max_input_channels"] == 1
    ]
    if len(devices) != 1:
        raise RuntimeError(
            f"Expected exactly one Flipper Dummy Mic audio input, found {len(devices)}"
        )
    port = find_port()
    request(port, "reset")
    request(port, "level 128")
    report = {}
    report["selected_device"] = {
        "name": devices[0][1]["name"],
        "index": devices[0][0],
        "hostapi": devices[0][1]["hostapi"],
    }
    try:
        for mode in ("noise", "silence"):
            request(port, mode)
            with sd.InputStream(
                device=devices[0][0], samplerate=16000, channels=1, dtype="int16"
            ) as stream:
                data, overflow = stream.read(16000)
                state = json.loads(request(port, "status"))
                values = data.astype(np.float64)
                rms = float(np.sqrt(np.mean(values**2)))
                report[mode] = {
                    "rms": rms,
                    "peak": int(np.max(np.abs(values))),
                    "overflow": overflow,
                    "status": state,
                }
                assert state["reading"] and state["session_ms"] >= 900, state
                assert not overflow, "Host audio buffer overflowed"
                if mode == "noise":
                    assert 60 < rms < 85 and report[mode]["peak"] <= 128, report[mode]
                else:
                    assert rms == 0, report[mode]
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = json.loads(request(port, "status"))
            if not state["reading"]:
                break
            time.sleep(0.2)
        assert not state["reading"], state
        time.sleep(0.4)
        final = json.loads(request(port, "status"))
        assert final["total_ms"] == state["total_ms"], (state, final)
        assert final["errors"] == 0 and final["rx_overflows"] == 0, final
        report["stopped"] = final
    finally:
        request(port, "noise")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
