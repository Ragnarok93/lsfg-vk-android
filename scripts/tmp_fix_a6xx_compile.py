#!/usr/bin/env python3
from pathlib import Path

# One-shot source repair for malformed newline literals introduced by patch generation.
path = Path('src/context.cpp')
text = path.read_text(encoding='utf-8')
broken = '              << "\n";'
fixed = '              << "\\n";'
count = text.count(broken)
if count != 2:
    raise SystemExit(f'expected 2 malformed newline literals, found {count}')
path.write_text(text.replace(broken, fixed), encoding='utf-8')
