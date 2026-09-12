from __future__ import annotations

def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)
