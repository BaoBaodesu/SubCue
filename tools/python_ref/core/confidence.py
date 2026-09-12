from __future__ import annotations


def calculate_confidence(similarity: float, ambiguity: float, length_ratio: float) -> float:
    value = similarity * 0.86 + min(1.0, length_ratio) * 0.09 + min(0.05, ambiguity) 
    return max(0.0, min(1.0, value))


def needs_ai_review(confidence: float, ambiguity: float = 1.0) -> bool:
    return confidence < 0.75 or ambiguity < 0.04
