from __future__ import annotations

import heapq
from dataclasses import dataclass

from rapidfuzz import fuzz

from core.anchors import Anchor, find_anchors
from core.confidence import calculate_confidence
from core.normalizer import normalize_text
from models.alignment_result import AlignmentResult
from models.subtitle import Subtitle
from models.transcript import TranscriptWord


@dataclass(frozen=True, slots=True)
class CharToken:
    char: str
    word_id: int
    start_ms: int
    end_ms: int


@dataclass(frozen=True, slots=True)
class Candidate:
    start: int
    end: int
    similarity: float
    score: float
    ambiguity: float = 1.0


def build_char_timeline(words: list[TranscriptWord]) -> list[CharToken]:
    timeline: list[CharToken] = []
    for word in words:
        normalized = normalize_text(word.text)
        if not normalized:
            continue
        duration = max(0, word.end_ms - word.start_ms)
        for index, char in enumerate(normalized):
            start = word.start_ms + round(duration * index / len(normalized))
            end = word.start_ms + round(duration * (index + 1) / len(normalized))
            timeline.append(CharToken(char, word.id, start, min(word.end_ms, max(start, end))))
    return timeline


def align_subtitles(lines: list[str], words: list[TranscriptWord]) -> AlignmentResult:
    timeline = build_char_timeline(words)
    stream = "".join(token.char for token in timeline)
    subtitles = [Subtitle(index + 1, text) for index, text in enumerate(lines)]
    if not timeline:
        return AlignmentResult(subtitles)
    anchors = find_anchors(lines, stream)
    anchor_map = {anchor.line_index: anchor for anchor in anchors}
    boundaries = [Anchor(-1, -1, -1, 1.0), *anchors, Anchor(len(lines), len(stream), len(stream), 1.0)]
    for left, right in zip(boundaries, boundaries[1:]):
        segment_indices = list(range(left.line_index + 1, right.line_index))
        if segment_indices:
            choices = _align_segment(lines, stream, segment_indices, left.end + 1, right.start)
            for line_index, candidate in zip(segment_indices, choices):
                _apply_candidate(subtitles[line_index], candidate, timeline, words)
        if right.line_index < len(lines):
            _apply_anchor(subtitles[right.line_index], anchor_map[right.line_index], timeline, words)
    resolve_timing(subtitles)
    return AlignmentResult(subtitles)


def _align_segment(lines: list[str], stream: str, indices: list[int], lower: int, upper: int) -> list[Candidate | None]:
    targets = [normalize_text(lines[index]) for index in indices]
    total_length = sum(max(1, len(target)) for target in targets)
    candidates_by_line: list[list[Candidate]] = []
    consumed = 0
    for target in targets:
        expected = lower + round((upper - lower) * consumed / max(1, total_length))
        expected_span = max(20, round((upper - lower) * max(1, len(target)) / max(1, total_length)))
        radius = max(100, expected_span * 3)
        search_left = max(lower, expected - radius)
        search_right = min(upper, expected + radius + max(1, len(target)))
        candidates_by_line.append(_find_candidates(target, stream, search_left, search_right, expected, upper - lower))
        consumed += max(1, len(target))
    beam: list[tuple[float, int, list[Candidate | None]]] = [(0.0, lower - 1, [])]
    for candidates in candidates_by_line:
        expanded: list[tuple[float, int, list[Candidate | None]]] = []
        for score, last_end, choices in beam:
            expanded.append((score - 0.38, last_end, [*choices, None]))
            for candidate in candidates:
                if candidate.start > last_end:
                    expanded.append((score + candidate.score, candidate.end, [*choices, candidate]))
        expanded.sort(key=lambda item: item[0], reverse=True)
        beam = expanded[:80]
    return max(beam, key=lambda item: item[0])[2]


def _find_candidates(target: str, stream: str, left: int, right: int, expected: int, region_length: int) -> list[Candidate]:
    if not target or right <= left:
        return []
    lengths = sorted({max(1, round(len(target) * ratio)) for ratio in (0.72, 0.86, 1.0, 1.14, 1.30)})
    scored: list[tuple[float, int, int, float]] = []
    for start in range(left, right):
        for length in lengths:
            end = start + length
            if end > right:
                continue
            similarity = fuzz.ratio(target, stream[start:end]) / 100
            length_penalty = abs(length - len(target)) / max(1, len(target)) * 0.12
            order_penalty = abs(start - expected) / max(1, region_length) * 0.08
            score = similarity - length_penalty - order_penalty
            item = (score, start, end - 1, similarity)
            if len(scored) < 240:
                heapq.heappush(scored, item)
            elif score > scored[0][0]:
                heapq.heapreplace(scored, item)
    ordered = [Candidate(start, end, similarity, score) for score, start, end, similarity in sorted(scored, reverse=True)]
    selected: list[Candidate] = []
    for item in ordered:
        if all(abs(item.start - other.start) > max(1, len(target) // 4) for other in selected):
            selected.append(item)
        if len(selected) == 12:
            break
    if not selected:
        return []
    ambiguity = max(0.0, selected[0].similarity - (selected[1].similarity if len(selected) > 1 else 0.0))
    return [Candidate(item.start, item.end, item.similarity, item.score, ambiguity) for item in selected]


def _apply_anchor(subtitle: Subtitle, anchor: Anchor, timeline: list[CharToken], words: list[TranscriptWord]) -> None:
    candidate = Candidate(anchor.start, anchor.end, anchor.confidence, anchor.confidence, 1.0)
    _apply_candidate(subtitle, candidate, timeline, words)
    subtitle.confidence = max(0.98, anchor.confidence)
    subtitle.status = "HIGH_CONFIDENCE_ANCHOR"


def _apply_candidate(subtitle: Subtitle, candidate: Candidate | None, timeline: list[CharToken], words: list[TranscriptWord]) -> None:
    if candidate is None:
        return
    first, last = timeline[candidate.start], timeline[candidate.end]
    subtitle.start_ms = first.start_ms
    subtitle.end_ms = last.end_ms
    subtitle.start_word_id = first.word_id
    subtitle.end_word_id = last.word_id
    subtitle.candidate_text = _candidate_words(words, first.word_id, last.word_id)
    subtitle.ambiguity = candidate.ambiguity
    target_length = len(normalize_text(subtitle.text))
    candidate_length = candidate.end - candidate.start + 1
    subtitle.confidence = calculate_confidence(candidate.similarity, candidate.ambiguity, min(target_length, candidate_length) / max(1, max(target_length, candidate_length)))
    if candidate.ambiguity < 0.04:
        subtitle.confidence = min(subtitle.confidence, 0.69)
    subtitle.status = "MATCHED" if subtitle.confidence >= 0.70 else "LOW_CONFIDENCE"


def _candidate_words(words: list[TranscriptWord], start_id: int, end_id: int) -> str:
    return "".join(word.text for word in words if start_id <= word.id <= end_id)


def resolve_timing(subtitles: list[Subtitle]) -> None:
    cursor = 0
    for subtitle in subtitles:
        if subtitle.end_ms <= subtitle.start_ms:
            continue
        if subtitle.start_ms < cursor:
            subtitle.start_ms = cursor
        if subtitle.end_ms - subtitle.start_ms < 250:
            subtitle.start_ms = 0
            subtitle.end_ms = 0
            subtitle.status = "SKIPPED_NO_AUDIO"
            subtitle.skip_reason = "有效语音区间不足 250ms"
            continue
        cursor = subtitle.end_ms


def finalize_audio_first(subtitles: list[Subtitle]) -> None:
    """只保留有可靠音频证据的字幕，不为缺失文稿插值时间。"""
    for subtitle in subtitles:
        if subtitle.status == "SKIPPED_NO_AUDIO":
            continue
        if subtitle.end_ms <= subtitle.start_ms:
            subtitle.start_ms = 0
            subtitle.end_ms = 0
            subtitle.status = "SKIPPED_NO_AUDIO"
            subtitle.skip_reason = subtitle.skip_reason or "音频中未找到对应片段"
        elif subtitle.confidence < 0.70:
            subtitle.start_ms = 0
            subtitle.end_ms = 0
            subtitle.status = "SKIPPED_NO_AUDIO"
            subtitle.skip_reason = subtitle.skip_reason or "没有达到可靠音频匹配阈值"
    resolve_timing(subtitles)
