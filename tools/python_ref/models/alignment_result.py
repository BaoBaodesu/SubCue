from __future__ import annotations

from dataclasses import dataclass, field

from models.subtitle import Subtitle


@dataclass(slots=True)
class AlignmentResult:
    subtitles: list[Subtitle] = field(default_factory=list)

    @property
    def high_count(self) -> int:
        return sum(item.confidence >= 0.85 for item in self.subtitles)

    @property
    def low_count(self) -> int:
        return sum(item.status not in ("UNMATCHED", "SKIPPED_NO_AUDIO") and item.confidence < 0.70 for item in self.subtitles)

    @property
    def unmatched_count(self) -> int:
        return sum(item.status in ("UNMATCHED", "SKIPPED_NO_AUDIO") for item in self.subtitles)

    @property
    def skipped_count(self) -> int:
        return sum(item.status == "SKIPPED_NO_AUDIO" for item in self.subtitles)

    @property
    def exportable_subtitles(self) -> list[Subtitle]:
        return [item for item in self.subtitles if item.is_timed and item.text.strip()]
