# Copyright 2025 The Helium Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.

import json
import os
import sys
import xml.etree.ElementTree as ET
from fnmatch import fnmatchcase
from sys import argv
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] /
                       'ui/webui/resources/tools'))
import generate_grd

# Build metadata and WebAssembly sources are not extension runtime assets.
EXCLUDED_FILES = (
    'BUILD.gn',
    'README.chromium',
    'generate_file_list.py',
    '*.wat',
)

ublock_src = Path(argv[1])
out_file = argv[2]

grdp_file = Path(argv[3])
root_gen_dir = Path(argv[4])
files = []
compressed_assets = ET.Element('grit-part')

for root, _, filenames in os.walk(ublock_src):
  for filename in filenames:
    path = Path(root) / filename
    relative = path.relative_to(ublock_src).as_posix()
    if any(fnmatchcase(relative, pattern) for pattern in EXCLUDED_FILES):
      continue
    if relative.startswith('assets/') and path.suffix in ('.txt', '.dat'):
      file_path = '${root_gen_dir}/' + Path(
          os.path.relpath(path, root_gen_dir)).as_posix()
      entry = ET.fromstring(generate_grd._generate_include_row(
          'ublock', relative, file_path, {}, 'ublock'))
      entry.set('compress', 'gzip')
      compressed_assets.append(entry)
    else:
      files.append(relative)

with open(out_file, 'w') as f:
  json.dump({'base_dir': ublock_src.as_posix(), 'files': files}, f)
ET.indent(compressed_assets)
ET.ElementTree(compressed_assets).write(grdp_file, encoding='utf-8',
                                       xml_declaration=True)
