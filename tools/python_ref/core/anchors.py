from __future__ import annotations

from dataclasses import dataclass

from rapidfuzz import fuzz

from core.normalizer import normalize_text


@dataclass(frozen=True, slots=True)
class Anchor:
    line_index: int
    start: int
    end: int
    confidence: float


def find_anchors(lines: list[str], stream: str) -> list[Anchor]:
    candidates: list[Anchor] = []
    for line_index, original in enumerate(lines):
        target = normalize_text(original)
        if len(target) < 3:
            continue
        positions: list[int] = []
        start = stream.find(target)
        while start >= 0:
            positions.append(start)
            start = stream.find(target, start + 1)
        if len(positions) == 1:
            candidates.append(Anchor(line_index, positions[0], positions[0] + len(target) - 1, 1.0))
            continue
        if positions or len(stream) > 80_000:
            continue
        expected = round(line_index / max(1, len(lines)) * len(stream))
        radius = max(200, len(stream) // max(1, len(lines)) * 3)
        left, right = max(0, expected - radius), min(len(stream), expected + radius)
        scored = [(fuzz.ratio(target, stream[pos : pos + len(target)]) / 100, pos) for pos in range(left, max(left, right - len(target) + 1))]
        scored.sort(reverse=True)
        if scored and scored[0][0] >= 0.92 and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.04):
            candidates.append(Anchor(line_index, scored[0][1], scored[0][1] + len(target) - 1, scored[0][0]))
    return _monotonic_subset(candidates)


def _monotonic_subset(anchors: list[Anchor]) -> list[Anchor]:
    if not anchors:
        return []
    best_score = [len(str(anchor.end - anchor.start + 1)) + anchor.confidence for anchor in anchors]
    previous = [-1] * len(anchors)
    for current, anchor in enumerate(anchors):
        weight = anchor.end - anchor.start + 1 + anchor.confidence
        best_score[current] = weight
        for prior in range(current):
            if anchors[prior].line_index < anchor.line_index and anchors[prior].end < anchor.start and best_score[prior] + weight > best_score[current]:
                best_score[current] = best_score[prior] + weight
                previous[current] = prior
    index = max(range(len(anchors)), key=best_score.__getitem__)
    selected: list[Anchor] = []
    while index >= 0:
        selected.append(anchors[index])
        index = previous[index]
    return list(reversed(selected))
