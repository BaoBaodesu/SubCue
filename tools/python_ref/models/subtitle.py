from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class Subtitle:
    index: int
    text: str
    start_ms: int = 0
    end_ms: int = 0
    confidence: float = 0.0
    source: str = "local"
    status: str = "UNMATCHED"
    start_word_id: int | None = None
    end_word_id: int | None = None
    candidate_text: str = ""
    ambiguity: float = 1.0
    skip_reason: str = ""

    @property
    def is_timed(self) -> bool:
        return self.status != "SKIPPED_NO_AUDIO" and self.end_ms > self.start_ms

    @property
    def confidence_level(self) -> str:
        if self.confidence >= 0.85:
            return "高"
        if self.confidence >= 0.70:
            return "中"
        return "低"
