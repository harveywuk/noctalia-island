"""Find UI text on screenshots for the smoke tests.

Settings captions, light labels on grey buttons and small card titles are at the edge of what
Tesseract reads at screen resolution, so a lookup tries several renderings of the same screenshot
until the text turns up: a 2x upscale as is, inverted, 3x thresholded, and inverted again in sparse
mode. Positions come back in screen pixels. Matching ignores letter case and punctuation that OCR
attaches to word edges ("<Back", "Monitor:"), tolerates a one-letter slip ("aisplays"), and lets
one-letter words match any single character.
"""
import csv
import io
import os
import pathlib
import subprocess
from difflib import SequenceMatcher

from PIL import Image, ImageOps

REPO = pathlib.Path(__file__).resolve().parents[1]
EDGE_MARKS = '<>‹›‘’“”"\'|.,:;'

# (name, scale, transform, page segmentation mode)
VARIANTS = (
    ('plain', 2, None, '11'),
    ('inverted', 2, 'invert', '11'),
    ('threshold', 3, 'threshold', '11'),
    ('sparse', 2, 'invert', '12'),
)


def _tessdata():
    return os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def _normalise(word):
    return word.lower().strip(EDGE_MARKS)


def words(path, variant='plain', work_dir=None):
    """OCR rows for one rendering of the screenshot at `path`, in screen pixels."""
    name, scale, transform, psm = next(v for v in VARIANTS if v[0] == variant)
    image = Image.open(path).convert('L')
    image = image.resize((image.width*scale, image.height*scale), Image.LANCZOS)
    if transform == 'invert':
        image = ImageOps.autocontrast(ImageOps.invert(image))
    elif transform == 'threshold':
        # Light text on grey rows only reads once it is black on white.
        image = image.point(lambda value: 0 if value > 140 else 255)
    work = pathlib.Path(work_dir or pathlib.Path(path).parent)
    rendered = work/f'ocr-{name}.png'
    image.save(rendered)
    tsv = subprocess.run(
        ['tesseract', str(rendered), 'stdout', '--tessdata-dir', _tessdata(), '--psm', psm,
         '-c', 'tessedit_create_tsv=1', '-c', f'user_defined_dpi={96*scale}'],
        capture_output=True, text=True, check=True).stdout
    rows = []
    for row in csv.DictReader(io.StringIO(tsv), delimiter='\t', quoting=csv.QUOTE_NONE):
        if not (row.get('text') or '').strip() or not (row.get('left') or '').isdigit():
            continue
        for key in ('left', 'top', 'width', 'height'):
            row[key] = int(row[key])//scale
        rows.append(row)
    return rows


def _same_line(a, b):
    return a['block_num'] == b['block_num'] and a['line_num'] == b['line_num']


def find(path, text, *, min_x=0, min_y=0, max_y=None, exact=False, starts_line=False,
         variants=None, work_dir=None):
    """Centre (x, y) of the first run of words matching `text`, or None.

    exact: the run must be a whole line, not part of a longer label ("180°" vs "Flipped + 180°").
    starts_line: the run must start its line.
    """
    target = [_normalise(w) for w in text.split()]
    for variant in variants or [v[0] for v in VARIANTS]:
        rows = words(path, variant, work_dir)
        for i in range(len(rows) - len(target) + 1):
            run = rows[i:i+len(target)]
            got = [_normalise(r['text']) for r in run]
            # A one-letter word is too short to compare ("a" often reads as "3"); the words around it
            # carry the match.
            close = all((len(t) <= 1 and len(g) <= 1) or SequenceMatcher(None, g, t).ratio() >= .75
                        for g, t in zip(got, target))
            inside = len(target) == 1 and not exact and target[0] and target[0] in got[0]
            if not (got == target or close or inside):
                continue
            first, last = run[0], run[-1]
            if first['left'] < min_x or first['top'] < min_y or (max_y is not None and first['top'] > max_y):
                continue
            if starts_line and i > 0 and _same_line(rows[i-1], first):
                continue
            if exact and ((i > 0 and _same_line(rows[i-1], first))
                          or (i+len(target) < len(rows) and _same_line(rows[i+len(target)], first))):
                continue
            x0, x1 = first['left'], last['left'] + last['width']
            return (x0 + x1)//2, first['top'] + first['height']//2
    return None
