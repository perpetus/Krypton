import sys
from pathlib import Path


ASSETS = (
    ("kTranscriptHtml", "transcript.html"),
    ("kTranscriptCss", "transcript.css"),
    ("kTranscriptJs", "transcript.js"),
    ("kTranscriptMojo", "transcript.mojom-webui.js"),
)


def main():
    output = Path(sys.argv[1])
    inputs = [Path(name) for name in sys.argv[2:]]
    lines = ["#include <string_view>\n\nnamespace krypton_agent::chat_assets {\n"]
    for symbol, name in ASSETS:
        data = next(item for item in inputs if item.name == name).read_text(encoding="utf-8")
        delimiter = "krypton_asset"
        if ")" + delimiter + '"' in data:
            raise ValueError(f"Unexpected resource delimiter in {name}")
        lines.append(f'inline constexpr std::string_view {symbol} = R"{delimiter}({data}){delimiter}";\n')
    lines.append("}  // namespace krypton_agent::chat_assets\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
