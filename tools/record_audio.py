import argparse
import base64
import binascii
from datetime import datetime
from pathlib import Path
import time
import wave

import serial
from serial.tools import list_ports


def connect(port, original_path, usb_serial):
    deadline = time.monotonic() + 20
    last_error = "ESP32 did not respond on USB"
    while time.monotonic() < deadline:
        try:
            if not port.is_open:
                port.port = original_path
                if usb_serial:
                    matches = [device.device for device in list_ports.comports() if device.serial_number == usb_serial]
                    if len(matches) != 1:
                        time.sleep(0.4)
                        continue
                    port.port = matches[0]
                port.open()
            port.write(b"p")
            ping_deadline = min(time.monotonic() + 0.8, deadline)
            while time.monotonic() < ping_deadline:
                if port.readline().strip() == b"REC READY":
                    return
        except serial.SerialException as exc:
            last_error = str(exc)
            port.close()
            time.sleep(0.4)
    raise RuntimeError(f"Could not connect to ESP32 over USB ({last_error})")


def discard_interrupted_capture(port):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if not line or line == b"PCM END":
            return
    raise RuntimeError("Previous recording did not finish after reconnecting USB")


def capture(port):
    port.write(b"r")
    deadline = time.monotonic() + 15
    expected_frames = 0
    pcm = bytearray()
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line.startswith(b"Mic:"):
            raise RuntimeError(line.decode(errors="replace"))
        if line.startswith(b"PCM START "):
            _, _, sample_rate, frames = line.split()
            if expected_frames or (sample_rate, frames) != (b"16000", b"48000"):
                raise RuntimeError("Unexpected recording format")
            expected_frames = int(frames)
            print("Recording!", flush=True)
        elif line == b"PCM END":
            if not expected_frames or len(pcm) != 2 * expected_frames:
                raise RuntimeError("Recording is incomplete")
            return pcm
        elif line.startswith(b"PCM "):
            _, offset, encoded = line.split()
            block = base64.b64decode(encoded, validate=True)
            if not expected_frames or int(offset) * 2 != len(pcm) or not block or len(block) % 2:
                raise RuntimeError("Missing or invalid audio block")
            pcm.extend(block)
            if len(pcm) > expected_frames * 2:
                raise RuntimeError("Unexpected extra audio data")
    if not expected_frames:
        raise RuntimeError("ESP32 acknowledged USB but did not start recording")
    raise RuntimeError(f"Recording stopped after {len(pcm) // 2} of {expected_frames} samples")


def main():
    parser = argparse.ArgumentParser(description="Record 3-second WAV clips from INMP441 over USB.")
    parser.add_argument("port", help="ESP32 USB serial port")
    parser.add_argument("label", choices=("clap", "double_clap", "other"))
    parser.add_argument("--count", type=int, default=1, help="Number of clips")
    parser.add_argument("--output", type=Path, default=Path("recordings"))
    parser.add_argument("--no-prompt", action="store_true", help="Start immediately for a bench check")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least 1")
    output_dir = args.output / args.label
    output_dir.mkdir(parents=True, exist_ok=True)

    port = serial.Serial(baudrate=115200, timeout=0.5, write_timeout=1)
    port.dtr = port.rts = False
    usb_serial = next((device.serial_number for device in list_ports.comports() if device.device == args.port), None)
    try:
        connect(port, args.port, usb_serial)
        for index in range(args.count):
            for attempt in range(2):
                if not args.no_prompt:
                    input(f"Clip {index + 1}/{args.count} ({args.label}). Press Enter, then wait for 'Recording!'. ")
                    for second in (3, 2, 1):
                        print(second, flush=True)
                        time.sleep(1)
                try:
                    pcm = capture(port)
                    break
                except serial.SerialException:
                    if attempt:
                        raise
                    print("ESP32 disconnected. Waiting for USB to retry this clip.", flush=True)
                    port.close()
                    connect(port, args.port, usb_serial)
                    discard_interrupted_capture(port)
            path = output_dir / f"{datetime.now():%Y%m%d-%H%M%S-%f}.wav"
            with path.open("xb") as output, wave.open(output, "wb") as wav:
                wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                wav.writeframes(pcm)
            print(path.resolve(), flush=True)
    except (OSError, RuntimeError, ValueError, binascii.Error) as exc:
        parser.exit(1, f"Recording failed: {exc}\n")
    finally:
        port.close()


if __name__ == "__main__":
    main()
