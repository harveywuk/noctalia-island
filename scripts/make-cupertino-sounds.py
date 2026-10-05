#!/usr/bin/env python3
"""Synthesise the Cupertino XDG sound theme: short, soft, glassy event sounds in a macOS spirit.

Every sound is generated here (no samples), so the theme can be rebuilt and shared freely. Events
the theme does not define fall back to freedesktop. Writes ~/.local/share/sounds/cupertino.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import wave

import numpy as np

RATE = 48000
# Modes of a struck glass or small bell, relative to the fundamental: inharmonic partials are what
# make a tone read as glass rather than as an electronic beep.
GLASS = ((1.0, 1.0, 1.0), (2.32, 0.32, 0.55), (4.25, 0.12, 0.33), (6.63, 0.05, 0.2))


def timeline(seconds):
    return np.arange(int(RATE * seconds)) / RATE


def envelope(t, attack, decay):
    rise = np.clip(t / attack, 0, 1) if attack > 0 else np.ones_like(t)
    return rise * np.exp(-t / decay)


def taper(signal, seconds=0.01):
    """Fade the last few milliseconds to zero, so a sound cut off while ringing does not click."""
    n = min(len(signal), int(RATE * seconds))
    out = signal.copy()
    out[len(out) - n:] *= np.linspace(1, 0, n)
    return out


def glass(freq, decay, seconds, attack=0.002):
    t = timeline(seconds)
    tone = np.zeros_like(t)
    for ratio, gain, decay_scale in GLASS:
        tone += gain * np.sin(2 * np.pi * freq * ratio * t) * envelope(t, attack, decay * decay_scale)
    return taper(tone)


def place(length, *parts):
    """Mix (start seconds, signal) parts into one buffer. A part that does not fit is an error,
    not a silent cut, so retuning a start or a length cannot truncate a note unnoticed."""
    out = np.zeros(int(RATE * length))
    for start, signal in parts:
        i = int(RATE * start)
        if i < 0 or i + len(signal) > len(out):
            raise ValueError(f'a {len(signal) / RATE:.3f} s part at {start} s does not fit in {length} s')
        out[i:i + len(signal)] += signal
    return out


def bandpassed_noise(seconds, low, high, rng):
    noise = rng.standard_normal(int(RATE * seconds))
    spectrum = np.fft.rfft(noise)
    freqs = np.fft.rfftfreq(len(noise), 1 / RATE)
    spectrum[(freqs < low) | (freqs > high)] = 0
    return np.fft.irfft(spectrum, len(noise))


def message():
    # Two quick glass notes a fifth apart (E6, B6), the second one softer.
    return place(0.8, (0, glass(1318.5, 0.22, 0.8)), (0.085, 0.7 * glass(1975.5, 0.2, 0.7)))


def volume():
    # A soft "pop": a sine that drops in pitch as it dies away, over about 80 ms.
    t = timeline(0.12)
    freq = 520 + 260 * np.exp(-t / 0.018)
    phase = 2 * np.pi * np.cumsum(freq) / RATE
    return np.sin(phase) * envelope(t, 0.001, 0.022)


def shutter(rng):
    # Two filtered clicks 70 ms apart, as a shutter opens and closes, over a short low thump.
    def click(seconds, low, high, decay):
        t = timeline(seconds)
        burst = bandpassed_noise(seconds, low, high, rng) * envelope(t, 0.0005, decay)
        return burst / np.abs(burst).max()
    t = timeline(0.06)
    thump = np.sin(2 * np.pi * 170 * t) * envelope(t, 0.001, 0.014)
    return place(0.22, (0, click(0.05, 1800, 7000, 0.007)), (0, 0.5 * thump),
                 (0.07, 0.75 * click(0.05, 1400, 5500, 0.009)), (0.07, 0.35 * thump))


def plug():
    # One bright, slow chime with an octave shimmer above it.
    return place(1.1, (0, glass(1046.5, 0.38, 1.1)), (0.004, 0.18 * glass(2093.0, 0.25, 1.0)))


def tink():
    # A single high glass tap, for the bell and alert dialogs.
    return glass(1760.0, 0.11, 0.45)


def complete():
    # A gentle rising pair (G5, D6), for finished tasks.
    return place(0.72, (0, 0.8 * glass(784.0, 0.16, 0.6)), (0.11, glass(1174.7, 0.2, 0.6)))


def finish(signal, peak_db):
    signal = taper(signal)
    return signal / np.abs(signal).max() * 10 ** (peak_db / 20)


def write_oga(path, signal):
    pcm = (np.clip(signal, -1, 1) * 32767).astype('<i2')
    stereo = np.repeat(pcm[:, None], 2, axis=1)
    with tempfile.NamedTemporaryFile(suffix='.wav') as tmp:
        with wave.open(tmp.name, 'wb') as out:
            out.setnchannels(2)
            out.setsampwidth(2)
            out.setframerate(RATE)
            out.writeframes(stereo.tobytes())
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', tmp.name, '-c:a', 'libvorbis', '-q:a', '6',
                        str(path)], check=True)


def build(theme):
    rng = np.random.default_rng(7)  # Fixed, so a rebuild produces the same sounds.
    stereo = theme / 'stereo'
    stereo.mkdir(parents=True)
    sounds = {
        'message-new-instant': (message(), -12),
        'audio-volume-change': (volume(), -16),
        'screen-capture': (shutter(rng), -10),
        'power-plug': (plug(), -12),
        'bell': (tink(), -13),
        'complete': (complete(), -13),
    }
    for name, (signal, peak) in sounds.items():
        write_oga(stereo / f'{name}.oga', finish(signal, peak))
    aliases = {'message': 'message-new-instant', 'camera-shutter': 'screen-capture',
               'dialog-warning': 'bell', 'dialog-information': 'bell', 'window-attention': 'bell'}
    for alias, target in aliases.items():
        (stereo / f'{alias}.oga').symlink_to(f'{target}.oga')
    # macOS stays quiet when the charger is removed.
    (stereo / 'power-unplug.disabled').touch()
    (theme / 'index.theme').write_text(
        '[Sound Theme]\nName=Cupertino\nComment=Soft glassy event sounds for Noctalia\n'
        'Inherits=freedesktop\nDirectories=stereo\n\n[stereo]\nOutputProfile=stereo\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sounds-dir', type=Path, default=Path.home() / '.local/share/sounds')
    args = parser.parse_args()
    theme = args.sounds_dir / 'cupertino'
    args.sounds_dir.mkdir(parents=True, exist_ok=True)
    # Build beside the theme and swap it in only once every sound has encoded: a failed run keeps
    # the previous theme, and files for events no longer in the list do not linger.
    with tempfile.TemporaryDirectory(dir=args.sounds_dir, prefix='.cupertino-') as staging:
        staging = Path(staging)
        try:
            build(staging / 'cupertino')
        except FileNotFoundError as error:
            parser.exit(1, f'ffmpeg is required to encode the sounds: {error}\n')
        except subprocess.CalledProcessError:
            parser.exit(1, 'ffmpeg could not encode the sounds (it needs libvorbis); '
                           f'{theme} is unchanged\n')
        if theme.exists():
            theme.rename(staging / 'previous')
        (staging / 'cupertino').rename(theme)
    print(theme)


if __name__ == '__main__':
    main()
