from __future__ import annotations

import unicodedata


def normalize_text(text: str) -> str:
    normalized = unicodedata.normalize("NFKC", text).lower()
    return "".join(char for char in normalized if not char.isspace() and not unicodedata.category(char).startswith(("P", "S")))
