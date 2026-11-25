import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import wave


DETECTOR = Path(__file__).resolve().parents[1] / "firmware" / "ClapButton"
REPLAY = r"""
#include "ClapDetector.h"
#include <cstdio>

int main() {
  ClapDetector detector;
  int16_t sample;
  unsigned pairs = 0;
  while (std::fread(&sample, sizeof(sample), 1, stdin) == 1) {
    pairs += detector.push(sample).pair;
  }
  std::printf("%u\n", pairs);
}
"""


def main():
    parser = argparse.ArgumentParser(description="Replay 16 kHz PCM16 WAV recordings through the firmware detector.")
    parser.add_argument("sessions", type=Path, nargs="+", help="Recording directories with clap, double_clap or other subdirectories")
    args = parser.parse_args()
    if not (DETECTOR / "ClapModel.h").is_file():
        parser.error("No firmware model. Train and export ClapModel.h before replaying recordings (see README).")
    for session in args.sessions:
        if not session.is_dir():
            parser.error(f"Directory not found: {session}")
    expected_pairs = {"double_clap": 1, "clap": 0, "other": 0}
    recordings = {
        label: sorted(path for session in args.sessions for path in (session / label).glob("*.wav"))
        for label in expected_pairs
    }
    if not any(recordings.values()):
        parser.error("No WAV files in clap, double_clap or other subdirectories")

    with TemporaryDirectory(prefix="clap-replay-") as directory:
        executable = Path(directory) / "replay"
        subprocess.run(
            ["c++", "-std=c++17", "-O2", "-I", str(DETECTOR), str(DETECTOR / "ClapDetector.cpp"),
             "-x", "c++", "-", "-o", str(executable)],
            input=REPLAY, text=True, check=True,
        )
        for label, paths in recordings.items():
            if not paths:
                continue
            expected = expected_pairs[label]
            correct = 0
            for path in paths:
                with wave.open(str(path), "rb") as wav:
                    if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (16000, 1, 2):
                        parser.error(f"Expected mono 16 kHz PCM16 WAV: {path}")
                    pcm = wav.readframes(wav.getnframes())
                pairs = int(subprocess.check_output([executable], input=pcm))
                if pairs == expected:
                    correct += 1
                else:
                    print(f"{path}: detected {pairs} pairs, expected {expected}")
            print(f"{label}: {correct}/{len(paths)}")


if __name__ == "__main__":
    main()
