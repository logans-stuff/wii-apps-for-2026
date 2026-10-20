#!/usr/bin/env python3
"""Generate the files embedded by the one-shot Meshtoad SD installer."""

import argparse
from pathlib import Path


def bytes_literal(data: bytes) -> str:
    """Format bytes compactly while keeping compiler input line lengths sane."""
    values = [str(value) for value in data]
    return ",\n".join(",".join(values[pos : pos + 32]) for pos in range(0, len(values), 32))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-dir", type=Path, required=True)
    parser.add_argument("--tiles-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tiles-only", action="store_true")
    parser.add_argument("--tile-zoom", type=int)
    args = parser.parse_args()

    app_names = ("boot.dol", "meta.xml", "icon.png")
    files = [] if args.tiles_only else [(name, args.app_dir / name) for name in app_names]
    if args.tiles_dir.is_dir():
        tile_root = args.tiles_dir / str(args.tile_zoom) if args.tile_zoom is not None else args.tiles_dir
        files.extend(
            (f"tiles/{path.relative_to(args.tiles_dir).as_posix()}", path)
            for path in sorted(tile_root.rglob("*.png"))
        )

    if not files:
        parser.error("no files selected")

    missing = [str(path) for _, path in files if not path.is_file()]
    if missing:
        parser.error("missing input file(s): " + ", ".join(missing))

    lines = [
        "#ifndef INSTALL_DATA_H",
        "#define INSTALL_DATA_H",
        "#include <stddef.h>",
    ]
    entries = []
    total = 0
    for index, (name, path) in enumerate(files):
        data = path.read_bytes()
        total += len(data)
        lines.extend(
            (
                f"static const unsigned char blob{index}[]={{",
                bytes_literal(data),
                "};",
            )
        )
        entries.append(f'  {{"{name}",blob{index},sizeof(blob{index})}},')

    lines.extend(
        (
            "struct ifile{const char*name;const unsigned char*data;size_t len;};",
            "static const struct ifile INSTALL_FILES[]={",
            *entries,
            "};",
            "#define INSTALL_NFILES (sizeof(INSTALL_FILES)/sizeof(INSTALL_FILES[0]))",
            f"#define INSTALL_TOTAL_BYTES {total}u",
            "#endif",
            "",
        )
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"embedded {len(files)} files ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
