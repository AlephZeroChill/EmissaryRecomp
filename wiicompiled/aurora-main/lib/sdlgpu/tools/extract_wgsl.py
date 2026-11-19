#!/usr/bin/env python3
"""Write every inline WGSL raw string (R\"\"\"( ... )\"\"\") in aurora's sources to OUT_DIR as <file>_<n>.wgsl.

    python3 extract_wgsl.py AURORA_LIB_DIR OUT_DIR
"""
import pathlib
import re
import sys

sys.stdout.reconfigure(line_buffering=True)

RAW = re.compile(r'R"""\((.*?)\)"""', re.S)


def main():
    lib, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for path in sorted(lib.rglob('*.cpp')):
        for n, m in enumerate(RAW.finditer(path.read_text(errors='replace'))):
            body = m.group(1)
            if '@vertex' not in body and '@fragment' not in body and '@compute' not in body:
                continue
            name = f'{path.stem}_{n}.wgsl'
            (out / name).write_text(body)
            print(name)


if __name__ == '__main__':
    main()
