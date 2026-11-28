"""Generate the in-browser render kit from local assets and pinned libraries."""

import gzip
import hashlib
import json
import sys
from pathlib import Path


def main():
    output, vendor_root, *owned_paths = map(Path, sys.argv[1:])
    manifest = json.loads((vendor_root / "sha256.json").read_text(encoding="utf-8"))
    assets = {
        (f"guides/{path.name}" if path.parent.name == "guides" else path.name):
        path.read_bytes() for path in owned_paths
    }
    for relative_path, expected in manifest.items():
        package, filename = relative_path.split("/")
        directory = vendor_root / package
        version = next(line.removeprefix("Version: ") for line in
                       (directory / "README.chromium").read_text(
                           encoding="utf-8").splitlines()
                       if line.startswith("Version: "))
        data = (directory / filename).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Wrong render kit digest for {relative_path}")
        assets[f"lib/{package}@{version}/{filename}"] = data
    lines = [
        "#include <cstdint>\n#include <string_view>\n",
        '#include "base/containers/span.h"\n',
        "namespace krypton_agent::chat_assets {\n",
        "struct KitAsset { std::string_view path; base::span<const uint8_t> gzip; };\n",
    ]
    entries = []
    for index, (name, data) in enumerate(sorted(assets.items())):
        symbol = f"kKit{index}"
        compressed = gzip.compress(data, mtime=0)
        lines.append(f"inline constexpr uint8_t {symbol}[] = {{\n")
        for start in range(0, len(compressed), 24):
            lines.append(",".join(str(byte) for byte in compressed[start:start + 24]) +
                         ",\n")
        lines.append("};\n")
        entries.append(f'{{"kit/1/{name}", {symbol}}},\n')
    lines.extend(["inline constexpr KitAsset kKitAssets[] = {\n", *entries,
                  "};\n}  // namespace krypton_agent::chat_assets\n"])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
