import argparse
from pathlib import Path
import wave

import numpy as np


RATE = 16000
HOP = 128
FFT_SIZE = 256
WINDOW = 2048
PRE_ROLL = 384
MIN_RMS = 65


def read_wave(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (RATE, 1, 2):
            raise ValueError(f"Expected mono 16 kHz PCM16 WAV: {path}")
        pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float32)
    filtered = np.zeros_like(pcm)
    previous = 0.0
    for i in range(1, len(pcm)):
        previous = 0.9615 * (previous + pcm[i] - pcm[i - 1])
        filtered[i] = previous
    return filtered


def candidates(signal):
    blocks = signal[: len(signal) // HOP * HOP].reshape(-1, HOP)
    rms = np.sqrt(np.mean(blocks**2, axis=1))
    # Keep complete windows and let the microphone settle at the beginning of the clip.
    eligible = np.arange(32, len(rms) - (WINDOW - PRE_ROLL) // HOP)
    if not len(eligible):
        return []
    valid = eligible[rms[eligible] >= max(MIN_RMS, 5 * np.median(rms[eligible]))]
    peaks = []
    for index in valid[np.argsort(-rms[valid])]:
        if all(abs(index - peak) >= 12 for peak in peaks):
            peaks.append(index)
    return [(int(index * HOP), float(rms[index])) for index in peaks]


def mel_filters():
    frequencies = np.fft.rfftfreq(FFT_SIZE, 1 / RATE)
    mel = lambda hz: 2595 * np.log10(1 + hz / 700)
    edges = 700 * (10 ** (np.linspace(mel(100), mel(7600), 18) / 2595) - 1)
    low, center, high = edges[:-2, None], edges[1:-1, None], edges[2:, None]
    return np.maximum(0, np.minimum((frequencies - low) / (center - low), (high - frequencies) / (high - center)))


MEL = mel_filters()
HANN = np.hanning(FFT_SIZE)


def features(signal, center):
    start = center - PRE_ROLL
    clip = signal[start : start + WINDOW]
    clip = clip / max(float(np.sqrt(np.mean(clip**2))), 1.0)
    frames = np.lib.stride_tricks.sliding_window_view(clip, FFT_SIZE)[::HOP]
    spectrum = abs(np.fft.rfft(frames * HANN, axis=1)) ** 2
    return np.log(np.maximum(spectrum @ MEL.T, 1e-10)).reshape(-1)


def fit(features_by_label, rng):
    positive, negative = features_by_label
    count = max(len(positive), len(negative))
    x = np.vstack((positive[rng.choice(len(positive), count)], negative[rng.choice(len(negative), count)]))
    y = np.r_[np.ones(count), np.zeros(count)]
    mean = x.mean(axis=0)
    scale = np.maximum(x.std(axis=0), 0.5)
    x = (x - mean) / scale
    w1 = rng.normal(0, 0.05, (240, 16))
    b1 = np.zeros(16)
    w2 = rng.normal(0, 0.1, 16)
    b2 = np.zeros(1)
    params = [w1, b1, w2, b2]
    moment = [np.zeros_like(param) for param in params]
    variance = [np.zeros_like(param) for param in params]
    for step in range(1, 401):
        hidden = np.maximum(x @ w1 + b1, 0)
        logits = hidden @ w2 + b2
        error = (1 / (1 + np.exp(-np.clip(logits, -40, 40))) - y) / len(y)
        dh = (error[:, None] * w2) * (hidden > 0)
        gradients = [x.T @ dh + 0.002 * w1, dh.sum(axis=0), hidden.T @ error + 0.002 * w2, np.array([error.sum()])]
        for i, (p, g) in enumerate(zip(params, gradients)):
            moment[i] = 0.9 * moment[i] + 0.1 * g
            variance[i] = 0.999 * variance[i] + 0.001 * g**2
            p -= 0.005 * (moment[i] / (1 - 0.9**step)) / (np.sqrt(variance[i] / (1 - 0.999**step)) + 1e-8)
    return {name: values.astype(np.float32) for name, values in zip(("mean", "scale", "w1", "b1", "w2", "b2"), (mean, scale, *params))}


def normalized_noise(size, backgrounds, rng):
    white = rng.normal(size=size)
    if rng.integers(4) == 0:
        noise = white
    elif rng.integers(3) == 0:
        time = np.arange(size) / RATE
        frequencies = rng.uniform(110, 1100, 4)
        phase = rng.uniform(0, 2 * np.pi, 4)
        noise = np.sin(time[:, None] * frequencies * (2 * np.pi) + phase) @ rng.uniform(0.1, 1, 4)
    elif rng.integers(2) == 0 or not backgrounds:
        frequencies = np.fft.rfftfreq(size, 1 / RATE)
        tilt = (np.maximum(frequencies, 100) / 100) ** (-rng.uniform(0.3, 1.1))
        noise = np.fft.irfft(np.fft.rfft(white) * tilt, n=size)
    else:
        background = backgrounds[rng.integers(len(backgrounds))]
        start = rng.integers(len(background) - size + 1)
        noise = background[start : start + size]
    return noise / max(float(np.sqrt(np.mean(noise**2))), 1e-6)


def add_noise_views(training, signal, center, views, backgrounds, rng):
    for index in range(views):
        start = center + rng.integers(-HOP, HOP + 1) - PRE_ROLL
        clip = signal[start : start + WINDOW].copy()
        if len(clip) != WINDOW:
            continue
        if index % 4 != 0:
            attack = clip[PRE_ROLL : PRE_ROLL + 2 * HOP]
            level = float(np.sqrt(np.mean(attack**2)))
            margin_db = rng.uniform(5, 31)
            noise = normalized_noise(WINDOW, backgrounds, rng)
            clip += noise * (level * 10 ** (-margin_db / 20))
        training.append(features(clip, PRE_ROLL))


def fit_with_noise(roots):
    rng = np.random.default_rng(408)
    sessions = []
    for root in roots:
        # Set aside every fourth original before collecting background crops or windows.
        sessions.append({
            label: [read_wave(path) for index, path in enumerate(sorted((root / label).glob("*.wav")))
                    if index % 4 != 3]
            for label in ("other", "clap")
        })
    backgrounds = [signal for session in sessions for signal in session["other"] if len(signal) >= WINDOW]
    claps = []
    other = []
    for session in sessions:
        for signal in session["other"]:
            for center, _ in candidates(signal)[:8]:
                add_noise_views(other, signal, center, 16, backgrounds, rng)
        for signal in session["clap"]:
            peaks = candidates(signal)
            if not peaks:
                continue
            center, amplitude = peaks[0]
            if amplitude < 100 or not 0.28 * RATE <= center <= 2.5 * RATE:
                continue
            add_noise_views(claps, signal, center, 30, backgrounds, rng)
            for tail, _ in peaks[1:]:
                if 0 < tail - center <= RATE // 4:
                    add_noise_views(other, signal, tail, 18, backgrounds, rng)
    if not claps or not other:
        raise ValueError("Need recordings with clear claps and negative examples with sharp sounds")
    print(f"Training windows: {len(claps)} claps, {len(other)} other sounds.")
    model = fit([np.stack(claps), np.stack(other)], rng)
    # Calibrated on the original WAV sets with the full firmware pair logic.
    model["min_score"] = np.float32(2)
    model["anchor_score"] = np.float32(4)
    return model


def accepted_peaks(signal, model):
    accepted = []
    for index, _ in candidates(signal):
        x = (features(signal, index) - model["mean"]) / model["scale"]
        logit = np.maximum(x @ model["w1"] + model["b1"], 0) @ model["w2"] + model["b2"][0]
        if logit >= 0:
            accepted.append(index)
    return sorted(accepted)


def pair_count(peaks):
    count = 0
    first = None
    blocked_until = 0
    for peak in peaks:
        if peak < blocked_until:
            continue
        if first is None or peak - first > 10400:
            first = peak
        elif peak - first >= 1920:
            count += 1
            first = None
            blocked_until = peak + 12800
    return count


def main():
    parser = argparse.ArgumentParser(description="Train and evaluate a pilot clap classifier from INMP441 WAV files.")
    parser.add_argument("data", type=Path, nargs="*", default=[Path("recordings")], help="One or more recording folders")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--augment-noise", action="store_true", help="Train with recorded and synthetic background noise")
    args = parser.parse_args()
    roots = list(dict.fromkeys(path.resolve() for path in args.data))
    for root in roots:
        if not root.is_dir():
            parser.error(f"Directory not found: {root}")
    if args.augment_noise:
        try:
            model = fit_with_noise(roots)
        except ValueError as exc:
            parser.error(str(exc))
        output = args.output or Path("models/clap_noise.npz")
        output.parent.mkdir(parents=True, exist_ok=True)
        np.savez(output, **model)
        print(output.resolve())
        return
    output = args.output or Path("models/clap_pilot.npz")
    rng = np.random.default_rng(16)
    training = [[], []]
    test = {}
    for cls, label in enumerate(("clap", "other")):
        clips = []
        for path in (path for root in roots for path in sorted((root / label).glob("*.wav"))):
            signal = read_wave(path)
            peaks = candidates(signal)
            if cls == 0 and not peaks:
                print(f"No clear transient: {path.name}")
                continue
            clips.append((path, signal, peaks))
        if len(clips) < 5:
            parser.error(f"Need at least five usable clips for {label}")
        order = rng.permutation(len(clips))
        test[label] = [clips[i][:2] for i in order[:2]]
        for index in order[2:]:
            _, signal, peaks = clips[index]
            for center, _ in peaks[: 1 if cls == 0 else 8]:
                for shift in rng.integers(-HOP, HOP + 1, 24):
                    training[cls].append(features(signal, center + shift))
            if cls == 0:
                # Include the room's reverberation after each annotated single clap.
                center = peaks[0][0]
                for tail, _ in peaks[1:]:
                    if 0 < tail - center <= RATE // 4:
                        for shift in rng.integers(-HOP, HOP + 1, 12):
                            training[1].append(features(signal, tail + shift))
        print(f"{label}: {len(clips) - 2} training files, 2 validation files")
    if not all(training):
        parser.error("Need negative examples with sharp sounds")
    model = fit([np.array(items) for items in training], rng)
    for root in roots:
        test[root / "double_clap"] = [(p, read_wave(p)) for p in sorted((root / "double_clap").glob("*.wav"))]
    for label, clips in test.items():
        correct = 0
        for path, signal in clips:
            peaks = accepted_peaks(signal, model)
            pairs = pair_count(peaks)
            correct += {"clap": len(peaks) == 1, "other": pairs == 0, "double_clap": pairs == 1}[Path(label).name]
            print(f"{path.parent.parent.name}/{path.parent.name}/{path.name}: claps {[round(i / RATE, 3) for i in peaks]}, pairs {pairs}")
        if clips:
            print(f"{label}: {correct}/{len(clips)}")
    output.parent.mkdir(parents=True, exist_ok=True)
    np.savez(output, **model)
    print(output.resolve())


if __name__ == "__main__":
    main()
