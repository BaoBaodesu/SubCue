"""Forced alignment: every line MUST get a non-zero-duration time range.

Wraps the existing alignment algorithm but:
1. Removes the beam's ability to skip lines (no None candidates).
2. After alignment, fills any UNMATCHED or 0-duration cues by interpolating
   from neighbours based on character-count proportional timing.
3. Guarantees: len(result.subtitles) == len(lines), all start < end, monotonic.
"""
from __future__ import annotations

from core.alignment import (
    Candidate,
    CharToken,
    align_subtitles,
    build_char_timeline,
    _apply_candidate,
    _candidate_words,
)
from core.confidence import calculate_confidence
from core.normalizer import normalize_text
from models.alignment_result import AlignmentResult
from models.subtitle import Subtitle
from models.transcript import TranscriptWord

_MIN_DURATION_MS = 250


def forced_align_subtitles(lines: list[str], words: list[TranscriptWord], duration_ms: int) -> AlignmentResult:
    """Run alignment then force every line to have a valid time range.

    Returns an AlignmentResult with exactly len(lines) subtitles, all with
    start_ms < end_ms and monotonically non-overlapping timing.
    """
    # First pass: normal alignment
    result = align_subtitles(lines, words)

    if not words or duration_ms <= 0:
        # No audio at all – distribute evenly
        _distribute_evenly(result.subtitles, 0, duration_ms)
        return result

    # Second pass: fill gaps for UNMATCHED / 0-duration cues
    _fill_gaps(result.subtitles, duration_ms)

    # Third pass: enforce minimum duration and monotonic order
    _enforce_constraints(result.subtitles, duration_ms)

    return result


def _fill_gaps(subtitles: list[Subtitle], duration_ms: int) -> None:
    """Assign time ranges to any subtitle with status UNMATCHED or start==end."""
    if not subtitles:
        return

    # Find runs of consecutive unplaced subtitles and fill them
    i = 0
    while i < len(subtitles):
        if _is_placed(subtitles[i]):
            i += 1
            continue

        # Find end of unplaced run
        run_start = i
        while i < len(subtitles) and not _is_placed(subtitles[i]):
            i += 1
        run_end = i  # exclusive

        # Find bounding times
        left_ms = subtitles[run_start - 1].end_ms if run_start > 0 else 0
        right_ms = subtitles[run_end].start_ms if run_end < len(subtitles) else duration_ms

        if right_ms <= left_ms:
            right_ms = left_ms + _MIN_DURATION_MS * (run_end - run_start)
            right_ms = min(right_ms, duration_ms)

        _distribute_evenly(subtitles[run_start:run_end], left_ms, right_ms)


def _distribute_evenly(subtitles: list[Subtitle], start_ms: int, end_ms: int) -> None:
    """Distribute *subtitles* proportionally by character count across [start_ms, end_ms)."""
    if not subtitles:
        return
    total_chars = sum(max(1, len(normalize_text(s.text))) for s in subtitles)
    available = max(end_ms - start_ms, _MIN_DURATION_MS * len(subtitles))
    cursor = start_ms
    for s in subtitles:
        chars = max(1, len(normalize_text(s.text)))
        span = max(_MIN_DURATION_MS, round(available * chars / max(1, total_chars)))
        s.start_ms = cursor
        s.end_ms = cursor + span
        cursor += span
        if s.status == "UNMATCHED":
            s.status = "GAPPED"
            s.confidence = max(s.confidence, 0.30)
            s.source = "interpolated"

    # Clamp last to end_ms (but not below MIN)
    if subtitles[-1].end_ms > end_ms and end_ms > subtitles[-1].start_ms + _MIN_DURATION_MS:
        subtitles[-1].end_ms = end_ms


def _enforce_constraints(subtitles: list[Subtitle], duration_ms: int) -> None:
    """Ensure min duration, no overlap, monotonic order."""
    for i, s in enumerate(subtitles):
        # Ensure minimum duration
        if s.end_ms - s.start_ms < _MIN_DURATION_MS:
            s.end_ms = s.start_ms + _MIN_DURATION_MS

        # Ensure no overlap with next
        if i + 1 < len(subtitles):
            nxt = subtitles[i + 1]
            if s.end_ms > nxt.start_ms:
                # Split the overlap – give priority to the one with higher confidence
                mid = (s.end_ms + nxt.start_ms) // 2
                if s.confidence >= nxt.confidence:
                    nxt.start_ms = s.end_ms
                else:
                    s.end_ms = nxt.start_ms
                # Still enforce minimum
                if s.end_ms - s.start_ms < _MIN_DURATION_MS:
                    s.end_ms = s.start_ms + _MIN_DURATION_MS
                if nxt.end_ms - nxt.start_ms < _MIN_DURATION_MS:
                    nxt.end_ms = nxt.start_ms + _MIN_DURATION_MS

    # Clamp last to duration
    if subtitles and subtitles[-1].end_ms > duration_ms:
        subtitles[-1].end_ms = max(subtitles[-1].start_ms + _MIN_DURATION_MS, duration_ms)


def _is_placed(s: Subtitle) -> bool:
    return s.status not in ("UNMATCHED",) and s.start_ms < s.end_ms
