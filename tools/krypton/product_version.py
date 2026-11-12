# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Krypton's Windows product version, independent of Chromium's engine version."""

import re

from krypton import layout


def parts():
    """Use the same canonical release marker for compilation and packaging."""
    values = layout.KRYPTON_VERSION.read_text(encoding='utf-8').strip().split('.')
    if len(values) != 4 or any(not re.fullmatch(r'0|[1-9][0-9]*', value)
                               or int(value) > 65535 for value in values):
        raise ValueError('version.txt must contain a canonical four-part Windows version')
    return dict(zip(('HELIUM_MAJOR', 'HELIUM_MINOR', 'HELIUM_PATCH',
                     'HELIUM_PLATFORM'), values))
