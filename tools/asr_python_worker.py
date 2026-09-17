#!/usr/bin/env python3
"""独立本地 ASR Worker；进程退出后释放模型及 CUDA 显存。"""
import json
import sys
from pathlib import Path
from zipfile import ZipFile
from xml.etree import ElementTree

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


def check(provider):
    import torch
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; local ASR requires an NVIDIA CUDA PyTorch runtime")
    if provider == "qwen3":
        import qwen_asr  # noqa: F401
    elif provider == "funasr":
        import funasr  # noqa: F401
    else:
        raise ValueError("unknown provider")


def qwen(model_dir, aligner_dir, chunks, progress=None):
    import gc
    import torch
    from qwen_asr import Qwen3ASRModel
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; Qwen3-ASR requires an NVIDIA CUDA PyTorch runtime")
    if progress:
        progress("load-asr", 0, 1, "正在加载 Qwen3-ASR 模型")
    model = Qwen3ASRModel.from_pretrained(model_dir, dtype=torch.float16,
        device_map="cuda:0", max_inference_batch_size=1, max_new_tokens=512)
    if progress:
        progress("load-asr", 1, 1, "Qwen3-ASR 模型已加载")
    recognized = []
    for index, chunk in enumerate(chunks):
        result = model.transcribe(audio=chunk["path"], language="Chinese")[0]
        recognized.append((chunk, getattr(result, "text", "")))
        if progress:
            progress("recognize", index + 1, len(chunks), "正在识别音频分块")
    # 8GB 显存下不得同时常驻 ASR 和 Forced Aligner。
    del model
    gc.collect()
    torch.cuda.empty_cache()
    segments = []
    if aligner_dir:
        from qwen_asr.inference.qwen3_forced_aligner import Qwen3ForcedAligner
        if progress:
            progress("load-aligner", 0, 1, "正在加载 Forced Aligner")
        aligner = Qwen3ForcedAligner.from_pretrained(aligner_dir, dtype=torch.float16, device_map="cuda:0")
        if progress:
            progress("load-aligner", 1, 1, "Forced Aligner 已加载")
        for index, (chunk, text) in enumerate(recognized):
            if not text.strip():
                continue
            aligned = aligner.align(audio=chunk["path"], text=text, language="Chinese")[0]
            for item in aligned.items:
                segments.append({"start": chunk["startMs"] / 1000 + item.start_time,
                                 "end": chunk["startMs"] / 1000 + item.end_time,
                                 "text": item.text})
            if progress:
                progress("align", index + 1, len(recognized), "正在生成字词时间")
    else:
        for chunk, text in recognized:
            # 没有 Forced Aligner 时只声明整个分块的粗范围，不能伪造 10ms 精确时间戳。
            if "endMs" not in chunk:
                import soundfile
                chunk["endMs"] = chunk["startMs"] + round(soundfile.info(chunk["path"]).duration * 1000)
            segments.append({"start": chunk["startMs"] / 1000,
                             "end": chunk["endMs"] / 1000,
                             "text": text, "timing": "chunk"})
    return segments


def funasr(model_dir, chunks):
    from funasr import AutoModel
    import torch
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; Fun-ASR Nano requires an NVIDIA CUDA PyTorch runtime")
    # 不声明 VAD 模型，避免 Fun-ASR 在离线运行时隐式下载额外权重。
    model = AutoModel(model=model_dir, trust_remote_code=True, hub="ms", device="cuda:0")
    segments = []
    for chunk in chunks:
        result = model.generate(input=chunk["path"], language="中文", itn=True, batch_size=1)[0]
        segments.extend({"start": (chunk["startMs"] + item.get("start", 0)) / 1000,
                         "end": (chunk["startMs"] + item.get("end", 0)) / 1000,
                         "text": item.get("sentence", "")} for item in result.get("sentence_info", []))
    return segments


def parse_docx(path):
    namespace = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
    with ZipFile(path) as archive:
        root = ElementTree.fromstring(archive.read("word/document.xml"))
    body = root.find(f"{namespace}body")
    lines = []
    paragraph_index = 0

    def paragraph_text(paragraph):
        parts = []
        for node in paragraph.iter():
            if node.tag == f"{namespace}t":
                parts.append(node.text or "")
            elif node.tag == f"{namespace}tab":
                parts.append("\t")
            elif node.tag in (f"{namespace}br", f"{namespace}cr"):
                parts.append("\n")
        return "".join(parts)

    def append_paragraph(paragraph, table):
        nonlocal paragraph_index
        paragraph_index += 1
        for text in paragraph_text(paragraph).split("\n"):
            lines.append({"lineNumber": len(lines) + 1, "paragraph": paragraph_index,
                          "table": table, "text": text})

    if body is not None:
        for child in body:
            if child.tag == f"{namespace}p":
                append_paragraph(child, False)
            elif child.tag == f"{namespace}tbl":
                for row in child.findall(f"{namespace}tr"):
                    for cell in row.findall(f"{namespace}tc"):
                        for paragraph in cell.findall(f"{namespace}p"):
                            append_paragraph(paragraph, True)
    return {"lines": lines}


def run_request(request, progress=None):
    if request.get("action") == "ping":
        return {"python": sys.version.split()[0]}
    if request.get("action") == "parse-docx":
        return parse_docx(request["path"])
    if request.get("action") == "check":
        check(request["provider"])
        return {"cuda": True, "provider": request["provider"]}
    segments = (qwen(request["modelDirectory"], request.get("forcedAlignerDirectory", ""),
                     request["chunks"], progress) if request["provider"] == "qwen3"
                else funasr(request["modelDirectory"], request["chunks"]))
    return {"segments": segments}


def stdio():
    for line in sys.stdin:
        if not line.strip():
            continue
        request = json.loads(line)
        task_id = request["taskId"]
        print(json.dumps({"taskId": task_id, "type": "accepted"}), flush=True)
        try:
            def progress(stage, completed, total, message):
                print(json.dumps({"taskId": task_id, "type": "progress", "stage": stage,
                                  "completed": completed, "total": total, "message": message},
                                 ensure_ascii=False), flush=True)
            result = run_request(request, progress)
            result_path = request.get("resultPath", "")
            if result_path:
                Path(result_path).write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
                result = {"resultPath": result_path}
            print(json.dumps({"taskId": task_id, "type": "result", **result},
                             ensure_ascii=False), flush=True)
        except Exception as error:
            print(json.dumps({"taskId": task_id, "type": "error", "message": str(error)},
                             ensure_ascii=False), flush=True)
            raise


def main():
    if sys.argv[1] == "--stdio":
        stdio()
        return
    if sys.argv[1] == "--check":
        check(sys.argv[2])
        print(json.dumps({"cuda": True, "provider": sys.argv[2]}))
        return
    request = json.load(open(sys.argv[1], encoding="utf-8"))
    print(json.dumps(run_request(request), ensure_ascii=False))


if __name__ == "__main__":
    main()
