from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class TranscriptWord:
    id: int
    text: str
    start_ms: int
    end_ms: int


@dataclass(slots=True)
class Transcript:
    words: list[TranscriptWord] = field(default_factory=list)

    def sort_and_reindex(self) -> None:
        self.words.sort(key=lambda word: (word.start_ms, word.end_ms))
        for index, word in enumerate(self.words, 1):
            word.id = index
