#!/usr/bin/env python3
"""Generate original Noctalia sound themes and a local listening page.

Requires NumPy and ffmpeg, like make-cupertino-sounds.py. Nothing is installed or selected.
Run from the checkout: python3 scripts/make-island-sound-previews.py
"""
import argparse
import json
from pathlib import Path
import subprocess
import wave
import zipfile

import numpy as np

RATE = 48000
STYLES = {
    'glass': ('Glass', 'Clear, delicate chimes with a little shimmer.', '#b9d9ff', 783.9909),
    'warm': ('Warm', 'Soft wooden notes and low, rounded taps.', '#ffd0a8', 523.2511),
    'playful': ('Playful', 'Bouncy electronic notes, bubbles and little chirps.', '#c5b5ff', 659.2551),
    'modular': ('Modular', 'Springy plucks, soft filter sweeps and tiny sequences.', '#a5e1c3', 329.6276),
}
EVENTS = (
    ('message-new-instant', 'Notification', -26),
    ('audio-volume-change', 'Volume', -31),
    ('screen-capture', 'Screenshot', -28),
    ('power-plug', 'Charging', -27),
    ('bell', 'Alert', -27),
    ('complete', 'Complete', -27),
)


def timeline(seconds):
    return np.arange(round(RATE * seconds)) / RATE


def envelope(t, attack, decay):
    return np.sin(np.minimum(t / attack, 1) * np.pi / 2) ** 2 * np.exp(-t / decay)


def fade(signal, seconds=.035):
    result = signal.copy()
    n = min(len(result), round(RATE * seconds))
    result[-n:] *= np.cos(np.linspace(0, np.pi / 2, n)) ** 2
    result[0] = result[-1] = 0
    return result


def noise(seconds, low, high, seed):
    t = timeline(seconds)
    rng = np.random.default_rng(seed)
    frequencies = np.fft.rfftfreq(len(t), 1 / RATE)
    band = np.clip((frequencies - low) / 250, 0, 1) * np.clip((high - frequencies) / 400, 0, 1)
    result = np.fft.irfft(np.fft.rfft(rng.standard_normal(len(t))) * band, len(t))
    return result / max(np.max(np.abs(result)), 1e-9)


def note(style, frequency, seconds=.65, decay=.15):
    t = timeline(seconds)
    phase = 2 * np.pi * frequency * t
    if style == 'glass':
        result = np.zeros_like(t)
        for ratio, gain, speed in ((1, 1, 1), (2.39, .23, .57), (4.62, .065, .32), (6.17, .018, .2)):
            result += gain * np.sin(phase * ratio) * envelope(t, .003, decay * speed)
    elif style == 'warm':
        result = (np.sin(phase) + .20 * np.sin(2 * phase) + .045 * np.sin(3 * phase))
        result *= envelope(t, .007, decay)
        result += .045 * noise(seconds, 350, 2400, 13) * envelope(t, .002, .012)
    else:
        # A small upward scoop and a decaying FM overtone give the note a rounded, bouncy onset.
        frequency_curve = frequency * (1 - .10 * np.exp(-t / .012))
        phase = 2 * np.pi * np.cumsum(frequency_curve) / RATE
        result = np.sin(phase + .65 * np.exp(-t / .038) * np.sin(2 * phase))
        result *= envelope(t, .003, decay)
    return fade(result)


def mix(*parts):
    # Allocate from the actual sample lengths, so no tail can be silently truncated.
    length = max(round(start * RATE) + len(signal) for start, signal in parts)
    result = np.zeros(length)
    for start, signal in parts:
        offset = round(start * RATE)
        result[offset:offset + len(signal)] += signal
    return fade(result)


def volume(style):
    t = timeline(.14)
    base, drop, decay = {'glass': (760, 300, .022), 'warm': (240, 170, .027),
                         'playful': (300, 520, .024)}[style]
    phase = 2 * np.pi * np.cumsum(base + drop * np.exp(-t / .014)) / RATE
    return fade(np.sin(phase) * envelope(t, .003, decay), .015)


def shutter(style):
    settings = {'glass': (1600, 6200, 1300, .038), 'warm': (350, 2600, 210, .055),
                'playful': (900, 4200, 650, .060)}
    low, high, frequency, gap = settings[style]
    t = timeline(.09)
    click = .65 * noise(.09, low, high, 47) * envelope(t, .0015, .006)
    click += .24 * np.sin(2 * np.pi * frequency * t) * envelope(t, .002, .015)
    if style == 'playful':
        click += .30 * np.sin(2 * np.pi * (1000 * t - 2200 * t * t)) * envelope(t, .002, .014)
    click = fade(click, .02)
    return mix((0, click), (gap, .64 * click))


def resonant_lowpass(signal, cutoff, resonance):
    """A time-varying state-variable filter, with trapezoidal integration for stability."""
    g = np.tan(np.pi * np.clip(cutoff, 80, 10000) / RATE)
    k = 1 / resonance
    a1 = 1 / (1 + g * (g + k))
    result = np.empty_like(signal)
    state1 = state2 = 0.
    for i, value in enumerate(signal):
        band = a1[i] * (state1 + g[i] * (value - state2))
        low = state2 + g[i] * band
        state1 = 2 * band - state1
        state2 = 2 * low - state2
        result[i] = low
    return result


def modular_note(frequency, seconds=.55, decay=.11, brightness=7., resonance=3.2, bend=1.3):
    t = timeline(seconds)
    pitch = frequency * 2 ** ((bend * np.exp(-t / .018) + .035 * np.sin(2 * np.pi * 3.1 * t)) / 12)
    phase = 2 * np.pi * np.cumsum(pitch) / RATE
    triangle = np.zeros_like(t)
    saw = np.zeros_like(t)
    pulse = np.zeros_like(t)
    width = .36 + .035 * np.sin(2 * np.pi * 2.7 * t)
    # Additive oscillators avoid discontinuities and keep every source partial below Nyquist.
    for harmonic in range(1, 17):
        if harmonic * np.max(pitch) >= RATE * .42:
            break
        saw += (-1) ** (harmonic + 1) * np.sin(harmonic * phase) / harmonic
        pulse += np.sin(np.pi * harmonic * width) * np.cos(harmonic * phase - np.pi * harmonic * width) / harmonic
        if harmonic % 2:
            triangle += (-1) ** ((harmonic - 1) // 2) * np.sin(harmonic * phase) / harmonic ** 2
    triangle *= 8 / np.pi ** 2
    source = .55 * triangle + .24 * saw + .18 * pulse
    source += .10 * np.sin(phase * 1.0025)  # A gently detuned second oscillator.
    # The opening filter and amplitude share an envelope, like a struck low-pass gate.
    # A little resonant emphasis gives the short events their rounded "plock".
    cutoff = frequency * (.72 + brightness * np.exp(-t / (decay * .48)))
    filtered = resonant_lowpass(source, cutoff, resonance)
    result = np.tanh(filtered * 1.25) * envelope(t, .0035, decay)
    return fade(result, .035)


def make_modular_sounds():
    root = STYLES['modular'][3]
    n = lambda ratio, **kwargs: modular_note(root * ratio, **kwargs)
    # Short, deliberate sequencer steps; each event gets its own pitch and filter contour.
    tick = n(3, seconds=.11, decay=.013, brightness=5., resonance=2.1, bend=-3.)
    burst_t = timeline(.11)
    burst = noise(.11, 1200, 5600, 91) * envelope(burst_t, .0015, .006)
    snap = fade(.60 * tick + .15 * burst, .025)
    ending = n(2, seconds=.70, decay=.16, brightness=5.2, resonance=2.5)
    return {
        'message-new-instant': mix(
            (0, n(1, seconds=.48, decay=.085, brightness=8.5)),
            (.115, .72 * n(1.5, seconds=.48, decay=.095, brightness=6.5))),
        'audio-volume-change': mix(
            (0, n(.875, seconds=.17, decay=.038, brightness=10., resonance=4.1, bend=8.)),
            (0, .17 * n(2, seconds=.075, decay=.009, brightness=5., resonance=2., bend=0.))),
        'screen-capture': mix((0, snap), (.048, .60 * snap)),
        'power-plug': mix(
            (0, .66 * n(.5, seconds=.65, decay=.12, brightness=9.)),
            (.11, .76 * n(1, seconds=.65, decay=.15, brightness=7.)),
            (.22, .60 * n(1.5, seconds=.78, decay=.18, brightness=5., resonance=2.5))),
        'bell': mix(
            (0, n(1.125, seconds=.36, decay=.065, brightness=8., resonance=3.8)),
            (.145, .72 * n(.875, seconds=.38, decay=.075, brightness=6.5, resonance=3.5))),
        'complete': mix(
            (0, .65 * n(1, seconds=.45, decay=.08)),
            (.095, .65 * n(1.5, seconds=.45, decay=.08)),
            (.19, ending), (.29, .13 * ending)),
    }


def make_sounds(style):
    if style == 'modular':
        return make_modular_sounds()
    root = STYLES[style][3]
    n = lambda ratio, seconds=.65, decay=.15: note(style, root * ratio, seconds, decay)
    return {
        'message-new-instant': mix((0, n(1)), (.10, .62 * n(1.5, .60, .14))),
        'audio-volume-change': volume(style),
        'screen-capture': shutter(style),
        'power-plug': mix((0, .65 * n(.75, .85, .23)), (.08, .80 * n(1, .85, .23)),
                          (.17, .50 * n(1.5, .90, .25))),
        'bell': mix((0, n(.875, .42, .09)), (.16, .65 * n(.75, .42, .09))),
        'complete': mix((0, .70 * n(1, .70, .18)), (.11, .70 * n(1.25, .70, .18)),
                        (.22, n(1.5, .90, .24))),
    }


def master(signal, target_db):
    if not np.all(np.isfinite(signal)) or np.max(np.abs(signal)) < 1e-9:
        raise ValueError('Sound must be finite and audible')
    # Match RMS over the audible part of each event, with headroom for transient peaks.
    active = signal[np.abs(signal) > np.max(np.abs(signal)) * .025]
    gain = 10 ** (target_db / 20) / np.sqrt(np.mean(active ** 2))
    gain = min(gain, 10 ** (-10 / 20) / np.max(np.abs(signal)))
    return fade(signal * gain, .015)


def write_wav(path, signal):
    pcm = np.rint(signal * 32767).astype('<i2')
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm.tobytes())


def encode(source, target, codec):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(source), '-map_metadata', '-1',
                    '-c:a', codec, '-q:a', '5', str(target)], check=True)


def build(output, styles=('glass', 'warm', 'playful'), title=None, description=None):
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'rate': RATE, 'events': [], 'styles': {}}
    if title:
        manifest['title'] = title
    if description:
        manifest['description'] = description
    for event, title, _ in EVENTS:
        manifest['events'].append({'id': event, 'title': title})
    for style in styles:
        title, description, color, _ = STYLES[style]
        name = 'noctalia-' + style
        theme = output / 'themes' / name
        sounds = theme / 'stereo'
        previews = output / 'preview' / style
        sounds.mkdir(parents=True, exist_ok=True)
        previews.mkdir(parents=True, exist_ok=True)
        raw = make_sounds(style)
        suite = [np.zeros(round(RATE * .25))]
        style_data = {'title': title, 'description': description, 'color': color, 'sounds': {}}
        for event, _, level in EVENTS:
            if style == 'modular' and event == 'audio-volume-change':
                level += 4  # Give the short feedback pop presence alongside the other modular events.
            signal = master(raw[event], level)
            path = previews / (event + '.wav')
            write_wav(path, signal)
            encode(path, sounds / (event + '.oga'), 'libvorbis')
            style_data['sounds'][event] = {
                'file': str(path.relative_to(output)),
                'seconds': round(len(signal) / RATE, 4),
                'peak_dbfs': round(20 * np.log10(np.max(np.abs(signal))), 2),
                'start': round(sum(len(part) for part in suite) / RATE, 4),
            }
            suite.extend([signal, np.zeros(round(RATE * .65))])
        suite_path = output / 'preview' / (style + '.wav')
        write_wav(suite_path, np.concatenate(suite))
        encode(suite_path, suite_path.with_suffix('.mp3'), 'libmp3lame')
        style_data['preview'] = f'preview/{style}.mp3'
        manifest['styles'][style] = style_data
        aliases = {'message': 'message-new-instant', 'camera-shutter': 'screen-capture',
                   'dialog-warning': 'bell', 'dialog-information': 'bell', 'window-attention': 'bell'}
        for alias, target in aliases.items():
            path = sounds / (alias + '.oga')
            if not path.is_symlink():
                path.symlink_to(target + '.oga')
        (sounds / 'power-unplug.disabled').touch()
        (theme / 'index.theme').write_text(
            f'[Sound Theme]\nName=Noctalia {title}\nComment={description}\n'
            'Inherits=freedesktop\nDirectories=stereo\n\n[stereo]\nOutputProfile=stereo\n')
        with zipfile.ZipFile(output / (name + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(theme.rglob('*')):
                if path.is_file():
                    archive.write(path, path.relative_to(theme.parent))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    template = Path(__file__).with_name('sound-preview.html').read_text()
    (output / 'index.html').write_text(template.replace('/* SOUND_DATA */', json.dumps(manifest)))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=Path('build-rishot/sound-audition'))
    parser.add_argument('--styles', nargs='+', choices=STYLES, default=['glass', 'warm', 'playful'])
    parser.add_argument('--title')
    parser.add_argument('--description')
    args = parser.parse_args()
    build(args.output_dir.resolve(), args.styles, args.title, args.description)
    print((args.output_dir / 'index.html').resolve())


if __name__ == '__main__':
    main()
