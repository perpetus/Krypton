"""Vendor pinned npm icon data for the offline Agent render kit.

Usage: python tools/krypton/vendor_render_icons.py LUCIDE_TGZ SIMPLE_ICONS_TGZ
"""

from collections import defaultdict
from hashlib import sha256
import json
from pathlib import Path
import re
import sys
import tarfile
from xml.etree import ElementTree


VENDOR = (Path(__file__).resolve().parents[2] / "chromium_src" /
          "third_party" / "krypton_agent_kit")
TARBALLS = {
    "lucide-static-1.81.0.tgz":
        "90c560f35032ea55a60beeedb51c09cf0cc1fa77cbcfcd541778a9105b362f48",
    "simple-icons-16.38.1.tgz":
        "8ad81af0009aa0935df4411080feb4547b72e13458bbcf4d803b259b1f078804",
}


def digest(data):
    return sha256(data).hexdigest()


def package_file(archive, name):
    return archive.extractfile(f"package/{name}").read()


def module(data):
    return ("// Generated from pinned npm icon data by vendor_render_icons.py.\n"
            f"export default {json.dumps(data, separators=(',', ':'))};\n").encode()


def lucide_files(archive):
    nodes = json.loads(package_file(archive, "icon-nodes.json"))
    return {"nodes.mjs": module(nodes), "LICENSE": package_file(archive, "LICENSE")}


def simple_icons_files(archive):
    metadata = package_file(archive, "data/simple-icons.json")
    groups = defaultdict(dict)
    for icon in sorted(json.loads(metadata), key=lambda icon: icon["slug"]):
        slug = icon["slug"]
        hex_color = icon["hex"]
        if (not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", slug) or
                not re.fullmatch(r"[A-F0-9]{6}", hex_color)):
            raise ValueError(f"Unexpected Simple Icons entry: {slug}")
        svg = ElementTree.fromstring(package_file(archive, f"icons/{slug}.svg"))
        paths = svg.findall("{http://www.w3.org/2000/svg}path")
        if svg.get("viewBox") != "0 0 24 24" or len(paths) != 1 or not paths[0].get("d"):
            raise ValueError(f"Unexpected Simple Icons geometry: {slug}")
        group = "0-9" if slug[0].isdigit() else slug[0]
        groups[group][slug] = [hex_color, paths[0].get("d")]
    return {
        **{f"{group}.mjs": module(icons) for group, icons in groups.items()},
        "LICENSE.md": package_file(archive, "LICENSE.md"),
        "DISCLAIMER.md": package_file(archive, "DISCLAIMER.md"),
        "simple-icons.json": metadata,
    }


def main(lucide_tarball, simple_icons_tarball):
    manifest_path = VENDOR / "sha256.json"
    assets = json.loads(manifest_path.read_text(encoding="utf-8"))
    source_hashes = {"npm_tarballs": {}, "unpacked_files": {}}
    pending = {}
    for package, tarball, read_files in (
        ("lucide-static", lucide_tarball, lucide_files),
        ("simple-icons", simple_icons_tarball, simple_icons_files),
    ):
        actual = digest(tarball.read_bytes())
        if TARBALLS.get(tarball.name) != actual or not tarball.name.startswith(package + "-"):
            raise ValueError(f"Wrong pinned npm tarball: {tarball.name}")
        source_hashes["npm_tarballs"][tarball.name] = actual
        assets = {name: sha for name, sha in assets.items() if not name.startswith(package + "/")}
        with tarfile.open(tarball) as archive:
            files = read_files(archive)
        for filename, data in files.items():
            name = f"{package}/{filename}"
            manifest = assets if filename.endswith(".mjs") else source_hashes["unpacked_files"]
            manifest[name] = digest(data)
            pending[name] = data

    for name, data in pending.items():
        output = VENDOR / name
        output.parent.mkdir(exist_ok=True)
        output.write_bytes(data)
    manifest_path.write_text(json.dumps(assets, separators=(",", ":"), sort_keys=True) + "\n",
                             encoding="utf-8", newline="\n")
    (VENDOR / "icon-source-sha256.json").write_text(
        json.dumps(source_hashes, indent=2, sort_keys=True) + "\n",
        encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main(*map(Path, sys.argv[1:]))
