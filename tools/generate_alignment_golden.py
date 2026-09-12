# -*- coding: utf-8 -*-
"""Phase 7 对齐迁移 golden 生成器。

用 tools/python_ref 中的最小 Python 对照实现（rapidfuzz 3.x、CPython
round/NFKC/lower 语义）导出：
1. tests/golden/alignment_cases.json —— C++ AlignmentEngine 的行为 golden；
2. src/core/alignment/unicode_filter_data.h —— 与 CPython 3.11 一致的
   空白/P/S 过滤码点区间表和 Cased 属性表（不再依赖 Qt 的 Unicode 版本）。

此脚本不是编辑器运行路径。重新生成需要本机 CPython 3.11 与 rapidfuzz：
    python -m pip install -r tools/requirements-golden.txt
    python tools/generate_alignment_golden.py
生成后请跑 Debug/Release CTest 确认 C++ 仍与 fixtures 一致。
"""
from __future__ import annotations

import json
import random
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent / "python_ref"))

from rapidfuzz import fuzz  # noqa: E402
from rapidfuzz.distance import Indel  # noqa: E402

from core.alignment import align_subtitles, finalize_audio_first  # noqa: E402
from core.forced_align import forced_align_subtitles  # noqa: E402
from core.normalizer import normalize_text  # noqa: E402
from models.transcript import TranscriptWord  # noqa: E402


def subtitle_to_dict(sub) -> dict:
    return {
        "text": sub.text,
        "startMs": sub.start_ms,
        "endMs": sub.end_ms,
        "confidence": sub.confidence,
        "source": sub.source,
        "status": sub.status,
        "startWordId": sub.start_word_id,
        "endWordId": sub.end_word_id,
        "candidateText": sub.candidate_text,
        "ambiguity": sub.ambiguity,
        "skipReason": sub.skip_reason,
    }


def words_to_dict(words: list[TranscriptWord]) -> list[dict]:
    return [
        {"id": w.id, "text": w.text, "startMs": w.start_ms, "endMs": w.end_ms}
        for w in words
    ]


def build_ratio_cases() -> list[dict]:
    cases = [
        ("", ""),
        ("", "a"),
        ("a", ""),
        ("a", "a"),
        ("a", "b"),
        ("ab", "ba"),
        ("abc", "abc"),
        ("abc", "abd"),
        ("abc", "abcd"),
        ("abcd", "abc"),
        ("this is a test", "this is a test!"),
        ("lewenstein", "levenshtein"),
        ("你好", "你好"),
        ("你好", "你早"),
        ("你好世界", "你好"),
        ("こんにちは", "こんばんは"),
        ("𠀀测试", "𠀁测试"),
        ("aaaaabbbbb", "cccccddddd"),
        ("abcdefghij", "abcdefghij"),
        ("abcdefghij", "bcdefghijk"),
        ("a" * 40, "a" * 40),
        ("a" * 40, "b" * 40),
        ("a" * 17, "a" * 17 + "b"),
        ("怪物猎人荒野", "怪物猎人旷野"),
    ]
    rng = random.Random(0x7A11)
    alphabets = ["ab", "abc", "abcd", "abcdef", "你好世界测试", "abcdef你好世界"]
    for _ in range(2000):
        alphabet = rng.choice(alphabets)
        n = rng.randint(0, 14)
        m = rng.randint(0, 14)
        a = "".join(rng.choice(alphabet) for _ in range(n))
        b = "".join(rng.choice(alphabet) for _ in range(m))
        cases.append((a, b))
    return [{"a": a, "b": b, "score": fuzz.ratio(a, b)} for a, b in cases]


def build_normalize_cases() -> list[dict]:
    inputs = [
        "《怪物猎人：荒野》DLC",
        "ＩＰｈｏｎｅ１８Ｐｒｏ 100%~",
        "",
        "   ",
        "\t\n\x0b\x0c\r 行",
        "ﬁﬂ①½™㍿",
        "İ",
        "Σ",
        "ΑΣ",
        "ΑΣΑ",
        "ΑΣΒ",
        "ΑΣ1",
        "ΑΣ ",
        "ΑΣ.Σ",
        "ΑΣ'Σ",
        "ΑΣ:Σ",
        "ΣΑ",
        "ΑΣ\u0301",
        "straße STRASSE",
        "Éclair café",
        "𝕏𝕐",
        "\u3000全角\u3000",
        "\u1680\u2000\u200a\u2028\u2029\u202f\u205f",
        "\x1c\x1d\x1e\x1f",
        "\u00a0\x85",
        "Hello，World！你好、世界。",
        "Ｈｅｌｌｏ　Ｗｏｒｌｄ",
        "\u0345\u0301\u200b",
        "a\u0300e\u0301i\u0302",
        "·…—–",
        "𠀀𠀁𠀂",
        "“引号”‘单引’「」",
        "\ud800",
        "test\u00adword",
        "ＢＩＧ　ｌｅｔｔｅｒｓ",
    ]
    return [{"input": value, "output": normalize_text(value)} for value in inputs]


def build_round_cases() -> list[dict]:
    inputs = [
        0.0, 0.25, 0.49, 0.49999999999999994, 0.5, 0.5000000000000001, 0.51,
        0.75, 1.0, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 62.5, 63.5, 124.5,
        125.5, 187.5, 188.5, 250.5, 251.5, 100.5, 101.5, 999.5, 1000.5,
        0.125, 0.375, 0.625, 0.875, 1.25, 2.75,
    ]
    return [{"input": value, "output": round(value)} for value in inputs]


def timed_words(values: list[str], step_ms: int = 500) -> list[TranscriptWord]:
    return [
        TranscriptWord(i, text, (i - 1) * step_ms, i * step_ms)
        for i, text in enumerate(values, 1)
    ]


def asr_fixture_words() -> list[TranscriptWord]:
    payload = json.loads((ROOT / "tests" / "golden" / "asr_response_redacted.json").read_text(encoding="utf-8"))
    words: list[TranscriptWord] = []
    for transcript in payload["transcripts"]:
        for word in transcript["words"]:
            words.append(TranscriptWord(word["id"], word["text"], word["beginTimeMs"], word["endTimeMs"]))
    return words


def build_align_cases() -> list[dict]:
    cases: list[dict] = []

    def add(name: str, lines: list[str], words: list[TranscriptWord]) -> None:
        result = align_subtitles(lines, words)
        cases.append({
            "name": name,
            "lines": lines,
            "words": words_to_dict(words),
            "subtitles": [subtitle_to_dict(s) for s in result.subtitles],
        })

    # 现有 Python 测试套件用例
    add(
        "line_can_span_multiple_asr_words",
        ["今天我们讨论《怪物猎人：荒野》DLC", "内容"],
        timed_words(["今天", "我们", "讨论", "怪物猎人", "荒野", "DLC", "内容"]),
    )
    add(
        "asr_segmentation_does_not_define_subtitles",
        ["这个角色目前的整体表现还是比较不错的"],
        timed_words(["这个角色目前的整体表现", "还是比较不错的"]),
    )
    add(
        "output_order_is_monotonic",
        ["开场", "相同内容", "中间", "相同内容", "结尾"],
        timed_words(["开场", "相同内容", "中间", "相同内容", "结尾"]),
    )
    add(
        "audio_first_missing_script_line",
        ["开场", "音频里完全没有的广告描述", "实际内容", "结尾"],
        [
            TranscriptWord(1, "开场", 0, 500),
            TranscriptWord(2, "实际内容", 500, 1200),
            TranscriptWord(3, "结尾", 1200, 1700),
        ],
    )
    # ASR golden fixture 输入（tests/golden/asr_response_redacted.json）
    add(
        "asr_golden_fixture",
        ["Hello 各位观众朋友们好", "未找到音频"],
        asr_fixture_words(),
    )
    # 空输入
    add("empty_lines", [], [])
    add("empty_words", ["第一行", "第二行"], [])
    add("empty_both", [], [])
    # 纯标点行与零时长词
    add(
        "punctuation_only_lines",
        ["……", "！！！", "abc"],
        [TranscriptWord(1, "abc", 100, 100), TranscriptWord(2, "abc", 200, 500)],
    )
    # 扩展 B 汉字与未配对 surrogate
    add(
        "cjk_ext_b",
        ["𠀀𠀁𠀂", "𠀀"],
        timed_words(["𠀀𠀁", "𠀂"], 300),
    )
    add(
        "unpaired_surrogate",
        ["a\ud800b"],
        [TranscriptWord(1, "a\ud800b", 0, 600)],
    )

    rng = random.Random(0x7A11)
    word_pool = [
        "今天", "我们", "讨论", "怪物猎人", "荒野", "dlc", "内容", "开场",
        "相同内容", "中间", "结尾", "实际内容", "广告", "描述", "这个角色",
        "目前", "整体表现", "还是", "比较", "不错", "的", "一", "二", "三",
        "四", "五", "hello", "world", "the", "quick", "brown", "fox",
        "测试", "语音", "识别", "文字", "音频", "完全", "不匹配", "第一行",
        "第二行", "第三行", "𠀀", "𠀁",
    ]
    for case_index in range(60):
        line_count = rng.randint(1, 12)
        lines: list[str] = []
        for _ in range(line_count):
            if rng.random() < 0.08:
                lines.append("")
                continue
            pieces = [rng.choice(word_pool) for _ in range(rng.randint(1, 6))]
            line = "".join(pieces)
            if rng.random() < 0.25:
                line += rng.choice(["。", "，", "！", "《》", " ", "、"])
            lines.append(line)
        word_count = rng.randint(0, 18)
        words: list[TranscriptWord] = []
        cursor = rng.randint(0, 200)
        for word_id in range(1, word_count + 1):
            start = cursor
            cursor += rng.randint(50, 700)
            end = cursor if rng.random() < 0.9 else start
            words.append(TranscriptWord(word_id, rng.choice(word_pool), start, end))
        add(f"random_{case_index}", lines, words)
    return cases


def build_finalize_cases() -> list[dict]:
    cases: list[dict] = []
    rng = random.Random(0x51A7)
    word_pool = [
        "今天", "我们", "讨论", "内容", "开场", "相同", "中间", "结尾",
        "实际", "广告", "描述", "hello", "world", "测试", "语音",
    ]
    for case_index in range(15):
        line_count = rng.randint(1, 10)
        lines = [
            "".join(rng.choice(word_pool) for _ in range(rng.randint(1, 5)))
            for _ in range(line_count)
        ]
        word_count = rng.randint(0, 12)
        words: list[TranscriptWord] = []
        cursor = rng.randint(0, 300)
        for word_id in range(1, word_count + 1):
            start = cursor
            cursor += rng.randint(100, 600)
            words.append(TranscriptWord(word_id, rng.choice(word_pool), start, cursor))
        result = align_subtitles(lines, words)
        finalize_audio_first(result.subtitles)
        cases.append({
            "name": f"finalize_random_{case_index}",
            "lines": lines,
            "words": words_to_dict(words),
            "subtitles": [subtitle_to_dict(s) for s in result.subtitles],
        })
    return cases


def build_forced_cases() -> list[dict]:
    cases: list[dict] = []

    def add(name: str, lines: list[str], words: list[TranscriptWord], duration_ms: int) -> None:
        result = forced_align_subtitles(lines, words, duration_ms)
        cases.append({
            "name": name,
            "lines": lines,
            "words": words_to_dict(words),
            "durationMs": duration_ms,
            "subtitles": [subtitle_to_dict(s) for s in result.subtitles],
        })

    add(
        "forced_all_lines_are_placed",
        ["今天我们讨论", "这是额外的行", "内容"],
        timed_words(["今天", "我们", "讨论", "内容"]),
        2000,
    )
    add(
        "forced_no_zero_duration",
        ["第一行", "第二行", "第三行"],
        timed_words(["完全不匹配的文字"]),
        1500,
    )
    add(
        "forced_monotonic_order",
        ["开场", "相同内容", "中间", "相同内容", "结尾"],
        timed_words(["开场", "相同内容", "中间", "相同内容", "结尾"]),
        2500,
    )
    add(
        "forced_count_equals_lines",
        ["一", "二", "三", "四", "五"],
        timed_words(["一", "二", "三"]),
        3000,
    )
    add("forced_empty_words", ["行A", "行B"], [], 2000)
    add("forced_empty_words_and_lines", [], [], 2000)
    add("forced_zero_duration", ["行A", "行B"], timed_words(["一", "二"]), 0)
    add("forced_negative_duration", ["行A", "行B"], [], -500)

    rng = random.Random(0x7F0A)
    word_pool = [
        "今天", "我们", "讨论", "内容", "开场", "相同内容", "中间", "结尾",
        "实际内容", "广告", "描述", "hello", "world", "测试", "语音",
    ]
    for case_index in range(30):
        line_count = rng.randint(1, 10)
        lines = [
            "".join(rng.choice(word_pool) for _ in range(rng.randint(1, 5)))
            for _ in range(line_count)
        ]
        word_count = rng.randint(0, 14)
        words: list[TranscriptWord] = []
        cursor = rng.randint(0, 300)
        for word_id in range(1, word_count + 1):
            start = cursor
            cursor += rng.randint(100, 600)
            words.append(TranscriptWord(word_id, rng.choice(word_pool), start, cursor))
        duration = rng.randint(500, 12000)
        add(f"forced_random_{case_index}", lines, words, duration)
    return cases


def build_unicode_ranges() -> tuple[list[tuple[int, int]], list[tuple[int, int]], list[tuple[int, int]]]:
    """生成 (drop, cased, case-ignorable) 区间。

    drop            = str.isspace() or Unicode 类别 P*/S*
    cased           = str.islower() or str.isupper() or str.istitle()
                      （对齐 CPython _PyUnicode_IsCased 用于 final sigma）
    case-ignorable  = 用 CPython str.lower() 的 Final_Sigma 规则实测：
                      夹在两个 Σ 之间时会被跳过、使前一个 Σ 变成非词尾 σ。
    """
    drop: list[tuple[int, int]] = []
    cased: list[tuple[int, int]] = []
    ignorable: list[tuple[int, int]] = []

    def emit(ranges: list[tuple[int, int]], start: int, end: int) -> None:
        if start is not None:
            ranges.append((start, end))

    drop_start = None
    cased_start = None
    ignorable_start = None
    for cp in range(0x110000):
        ch = chr(cp)
        is_drop = ch.isspace() or unicodedata.category(ch).startswith(("P", "S"))
        is_cased = ch.islower() or ch.isupper() or ch.istitle()
        is_ignorable = False
        if not is_cased:
            probe = "\u0391\u03A3" + ch + "\u03A3"
            lowered = probe.lower()
            is_ignorable = len(lowered) > 1 and ord(lowered[1]) == 0x03C3
        if is_drop and drop_start is None:
            drop_start = cp
        if not is_drop and drop_start is not None:
            emit(drop, drop_start, cp - 1)
            drop_start = None
        if is_cased and cased_start is None:
            cased_start = cp
        if not is_cased and cased_start is not None:
            emit(cased, cased_start, cp - 1)
            cased_start = None
        if is_ignorable and ignorable_start is None:
            ignorable_start = cp
        if not is_ignorable and ignorable_start is not None:
            emit(ignorable, ignorable_start, cp - 1)
            ignorable_start = None
    emit(drop, drop_start, 0x10FFFF)
    emit(cased, cased_start, 0x10FFFF)
    emit(ignorable, ignorable_start, 0x10FFFF)
    return drop, cased, ignorable


def write_unicode_filter_header(
    drop: list[tuple[int, int]],
    cased: list[tuple[int, int]],
    ignorable: list[tuple[int, int]],
) -> None:
    lines = [
        "// 自动生成，请勿手工编辑。",
        r"// 生成命令：python tools/generate_alignment_golden.py",
        "// 语义与 CPython 3.11 (unicodedata 14.0.0) 一致：",
        "//   drop            = str.isspace() or unicodedata.category(ch).startswith((\"P\", \"S\"))",
        "//   cased           = str.islower() or str.isupper() or str.istitle()",
        "//   case-ignorable  = CPython Final_Sigma 会跳过的码点（含 Other_Case_Ignorable）",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace subcue::alignment::detail {",
        "",
        "struct CodepointRange final {",
        "    char32_t first;",
        "    char32_t last;",
        "};",
        "",
        "inline constexpr CodepointRange kNormalizeDropRanges[] = {",
    ]
    for first, last in drop:
        lines.append(f"    {{0x{first:X}, 0x{last:X}}},")
    lines += [
        "};",
        "",
        "inline constexpr CodepointRange kCasedRanges[] = {",
    ]
    for first, last in cased:
        lines.append(f"    {{0x{first:X}, 0x{last:X}}},")
    lines += [
        "};",
        "",
        "inline constexpr CodepointRange kCaseIgnorableRanges[] = {",
    ]
    for first, last in ignorable:
        lines.append(f"    {{0x{first:X}, 0x{last:X}}},")
    lines += [
        "};",
        "",
        "} // namespace subcue::alignment::detail",
        "",
    ]
    target = ROOT / "src" / "core" / "alignment" / "unicode_filter_data.h"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> None:
    drop, cased, ignorable = build_unicode_ranges()
    write_unicode_filter_header(drop, cased, ignorable)
    print(
        f"unicode_filter_data.h: {len(drop)} drop ranges, {len(cased)} cased ranges, "
        f"{len(ignorable)} case-ignorable ranges"
    )

    document = {
        "meta": {
            "generatedBy": "tools/generate_alignment_golden.py",
            "pythonVersion": sys.version.split()[0],
            "unicodedataVersion": unicodedata.unidata_version,
            "description": "SubCue Phase 7 自动对齐 C++ 实现的行为 golden，由 tools/python_ref 对照脚本生成。",
        },
        "ratioCases": build_ratio_cases(),
        "normalizeCases": build_normalize_cases(),
        "roundCases": build_round_cases(),
        "alignCases": build_align_cases(),
        "finalizeCases": build_finalize_cases(),
        "forcedCases": build_forced_cases(),
    }
    target = ROOT / "tests" / "golden" / "alignment_cases.json"
    target.write_text(
        json.dumps(document, ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(
        f"alignment_cases.json: {len(document['ratioCases'])} ratio, "
        f"{len(document['normalizeCases'])} normalize, {len(document['roundCases'])} round, "
        f"{len(document['alignCases'])} align, {len(document['finalizeCases'])} finalize, "
        f"{len(document['forcedCases'])} forced"
    )


if __name__ == "__main__":
    main()
