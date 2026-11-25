#!/usr/bin/env python3
"""Set and verify the StopWatch RTC in Japan Standard Time over USB."""

import argparse
import re
import time
from datetime import datetime, timedelta, timezone

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="StopWatch serial port, e.g. /dev/cu.usbmodemXXXX")
    args = parser.parse_args()
    jst = timezone(timedelta(hours=9))
    try:
        with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=1) as device:
            deadline = time.monotonic() + 16
            next_send = time.monotonic()
            while time.monotonic() < deadline:
                if time.monotonic() >= next_send:
                    device.write(f"JST_EPOCH {int(time.time())}\n".encode("ascii"))
                    next_send = time.monotonic() + 3
                line = device.readline().decode("ascii", errors="replace").strip()
                match = re.fullmatch(r"PICS clock set (.{19}) JST", line)
                if match:
                    result = datetime.strptime(match[1], "%Y/%m/%d %H:%M:%S").replace(tzinfo=jst)
                    difference = abs(time.time() - result.timestamp())
                    if difference <= 4:
                        print(f"Verified: {result.isoformat(sep=' ')}")
                        return 0
    except (serial.SerialException, OSError) as error:
        parser.exit(1, f"Serial error: {error}\n")
    parser.exit(1, "Clock not verified. Check that StopWatch is running normally and powered by USB.\n")


if __name__ == "__main__":
    raise SystemExit(main())
