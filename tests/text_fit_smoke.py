"""Settings text fit across translations, UI scale and reduced motion.

Every Settings page is opened directly (`settings-open <section>/<group>`) with
NOCTALIA_DEBUG_TEXT_FIT=1, which makes each ellipsized label log a `text-fit:` line.
A baseline pass in English at 1x is compared with stress passes, and labels newly cut
short are reported per page with a screenshot. Labels that are designed to truncate
(paths, long values) also truncate in the baseline, so only the difference is reported.
"""
import csv
import io
import json
import os
import pathlib
import re
import time

PASSES = [
    # name, [shell] lang, [accessibility] ui_scale, [shell.animation] enabled
    ('baseline', 'en', 1.0, True),
    ('de-1.5x', 'de', 1.5, False),
    ('en-1.5x', 'en', 1.5, False),
]
# NOCTALIA_TEXT_FIT_LANGS=ru,fr,ja replaces the stress passes with one 1x pass per language.
if os.environ.get('NOCTALIA_TEXT_FIT_LANGS'):
    PASSES = PASSES[:1] + [(lang, lang, 1.0, True)
                           for lang in os.environ['NOCTALIA_TEXT_FIT_LANGS'].split(',') if lang]

TEXT_FIT = re.compile(r"text-fit: ellipsized '(.*)' \(budget")


def prepare(base, cfg, env):
    env['NOCTALIA_DEBUG_TEXT_FIT'] = '1'


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    binary = os.environ.get('NOCTALIA_TEST_BINARY', str(repo/'build-rishot/noctalia'))
    log = out/'noctalia.log'
    config = cfg/'config.toml'
    original = config.read_text()
    pages = [line for line in run([binary, 'config', 'settings-pages']).splitlines() if '/' in line]
    assert len(pages) > 50, pages

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)]); return path

    def ocr_text(name):
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        result = run(['tesseract', str(shot(name)), 'stdout', '--tessdata-dir', tessdata, '--psm', '11',
                      '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=96'])
        rows = csv.DictReader(io.StringIO(result), delimiter='\t', quoting=csv.QUOTE_NONE)
        return ' '.join(r['text'] for r in rows if r.get('text', '').strip()).lower()

    def configure(lang, scale, animations):
        text = original.replace('[shell]\n', f'[shell]\nlang="{lang}"\n', 1)
        text += f'\n[accessibility]\nui_scale={scale}\n[shell.animation]\nenabled={str(animations).lower()}\n'
        config.write_text(text)
        msg('config-reload'); time.sleep(1.5)

    def collect(name):
        results = {}
        for page in pages:
            offset = log.stat().st_size
            msg('settings-open', page)
            time.sleep(.5 if name == 'baseline' else .7)
            with log.open(errors='replace') as f:
                f.seek(offset)
                results[page] = sorted(set(TEXT_FIT.findall(f.read())))
        return results

    report = {}
    try:
        dispatch('hl.dsp.focus({monitor="TEST-1"})')
        for name, lang, scale, animations in PASSES:
            configure(lang, scale, animations)
            msg('settings-close'); msg('settings-open', 'appearance'); time.sleep(1.2)
            seen = ocr_text(f'text-fit-{name}-overview')
            if lang == 'de':
                assert 'erscheinungsbild' in seen, 'German catalog did not load: '+seen[:200]
            report[name] = collect(name)
            msg('settings-close')
    finally:
        config.write_text(original)
        msg('config-reload')

    baseline = report['baseline']
    findings = {}
    for name, _, _, _ in PASSES[1:]:
        for page, labels in report[name].items():
            # Translated labels never match English text, so compare counts per page.
            extra = len(labels) - len(baseline.get(page, []))
            if extra > 0:
                findings.setdefault(name, {})[page] = labels
    (out/'text-fit.json').write_text(json.dumps({'pages': pages, 'passes': report, 'findings': findings},
                                                ensure_ascii=False, indent=2))

    # Re-open flagged pages for a visual record of each finding, plus any pages named in
    # NOCTALIA_TEXT_FIT_SHOTS (comma-separated) for reviewing layout changes.
    extra_shots = [p for p in os.environ.get('NOCTALIA_TEXT_FIT_SHOTS', '').split(',') if p]
    for name, lang, scale, animations in PASSES[1:]:
        shots = list(findings.get(name, {})) + [p for p in extra_shots if p not in findings.get(name, {})]
        if not shots:
            continue
        configure(lang, scale, animations)
        for page in shots:
            msg('settings-open', page); time.sleep(.8)
            shot(f'text-fit-{name}-{page.replace("/", "-")}')
        msg('settings-close')
    config.write_text(original); msg('config-reload')

    print(f'Text fit: {len(pages)} pages, baseline ellipsized on {sum(1 for v in baseline.values() if v)} pages',
          flush=True)
    for name, pages_found in findings.items():
        print(f'{name}: {len(pages_found)} pages with newly ellipsized labels', flush=True)
        for page, labels in sorted(pages_found.items()):
            print(f'  {page}: ' + ' | '.join(labels[:6]) + (' …' if len(labels) > 6 else ''), flush=True)
    print('PASS: text fit audit completed; see text-fit.json', flush=True)
