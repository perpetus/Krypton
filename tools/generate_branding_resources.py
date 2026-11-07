"""Generate native logo assets from Krypton's existing product artwork.

Pillow is already part of the pinned resource-generation toolchain. This adds
no dependency. The monochrome geometry uses the three strokes in product_logo.svg.
"""

from math import hypot
from pathlib import Path
import shutil
import sys

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from krypton import layout


def vector_icon(colored):
    lines = ['// Copyright 2026 The Krypton Authors',
             '// Generated from product_logo.svg by tools/generate_branding_resources.py.',
             'CANVAS_DIMENSIONS, 24,']
    scale = 24 / 1011.7
    radius = 130 * scale
    kappa = 0.5522847498307936

    def point(x, y):
        return ((x - 127.05) * scale, (y - 105.45) * scale)

    def emit(command, *points):
        numbers = ', '.join(f'{v:.4f}f' for p in points for v in p)
        lines.append(command + ', ' + numbers + ',')

    for (start, end), color in zip(
            [((367, 303), (367, 918)), ((555, 610), (897, 923)), ((493, 644), (899, 300))],
            ['0xFF, 0x5D, 0x6F, 0xFF', '0xFF, 0x96, 0x68, 0xED', '0xFF, 0x65, 0xCE, 0xF5']):
        a, b = point(*start), point(*end)
        length = hypot(b[0] - a[0], b[1] - a[1])
        t = ((b[0] - a[0]) / length, (b[1] - a[1]) / length)
        n = (-t[1], t[0])

        def at(center, tangent, normal):
            return (center[0] + radius * (tangent * t[0] + normal * n[0]),
                    center[1] + radius * (tangent * t[1] + normal * n[1]))

        lines += ['NEW_PATH,', 'FILL_RULE_NONZERO,']
        if colored:
            lines.append('PATH_COLOR_ARGB, ' + color + ',')
        emit('MOVE_TO', at(a, 0, 1))
        emit('LINE_TO', at(b, 0, 1))
        emit('CUBIC_TO', at(b, kappa, 1), at(b, 1, kappa), at(b, 1, 0))
        emit('CUBIC_TO', at(b, 1, -kappa), at(b, kappa, -1), at(b, 0, -1))
        emit('LINE_TO', at(a, 0, -1))
        emit('CUBIC_TO', at(a, -kappa, -1), at(a, -1, -kappa), at(a, -1, 0))
        emit('CUBIC_TO', at(a, -1, kappa), at(a, -kappa, 1), at(a, 0, 1))
        lines.append('CLOSE,')
    return '\n'.join(lines) + '\n'


def main():
    owned = layout.KRYPTON_RESOURCES / 'core'
    for name, colored in [('product_logo.icon', False), ('product_logo_color.icon', True)]:
        (owned / 'branding' / name).write_text(vector_icon(colored), encoding='utf-8')
    source = owned / 'branding' / 'product_logo_200.png'
    for upstream in (layout.HELIUM / 'resources' / 'favicons').glob('*.png'):
        target = owned / 'favicons' / upstream.name
        if target.exists():
            continue
        with Image.open(upstream) as reference:
            size = reference.size
        existing = owned / 'favicons' / f'favicon_ntp_{size[0]}.png'
        if existing.exists():
            shutil.copyfile(existing, target)
        else:
            with Image.open(source) as image:
                image.convert('RGBA').resize(size, Image.Resampling.LANCZOS).save(target)
    onboarding = (layout.KRYPTON_CHROMIUM_SRC / 'components' /
                  'helium_onboarding' / 'public' / 'favicon.png')
    onboarding.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(owned / 'favicons' / 'favicon_ntp_32.png', onboarding)
    print('Generated Krypton vector icons and internal-page favicons')


if __name__ == '__main__':
    main()
