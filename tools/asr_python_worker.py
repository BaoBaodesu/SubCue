#!/usr/bin/env python3
"""独立本地 ASR Worker；进程退出后释放模型及 CUDA 显存。"""
import json
import sys


def check(provider):
    if provider == "qwen3":
        import qwen_asr  # noqa: F401
    elif provider == "funasr":
        import funasr  # noqa: F401
    else:
        raise ValueError("unknown provider")


def qwen(model_dir, aligner_dir, chunks):
    import gc
    import torch
    from qwen_asr import Qwen3ASRModel
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; Qwen3-ASR requires an NVIDIA CUDA PyTorch runtime")
    model = Qwen3ASRModel.from_pretrained(model_dir, dtype=torch.float16,
        device_map="cuda:0", max_inference_batch_size=1, max_new_tokens=256)
    recognized = []
    for chunk in chunks:
        result = model.transcribe(audio=chunk["path"], language="Chinese")[0]
        recognized.append((chunk, getattr(result, "text", "")))
    # 8GB 显存下不得同时常驻 ASR 和 Forced Aligner。
    del model
    gc.collect()
    torch.cuda.empty_cache()
    segments = []
    if aligner_dir:
        from qwen_asr.inference.qwen3_forced_aligner import Qwen3ForcedAligner
        aligner = Qwen3ForcedAligner.from_pretrained(aligner_dir, dtype=torch.float16, device_map="cuda:0")
        for chunk, text in recognized:
            if not text.strip():
                continue
            aligned = aligner.align(audio=chunk["path"], text=text, language="Chinese")[0]
            for item in aligned.items:
                segments.append({"start": chunk["startMs"] / 1000 + item.start_time,
                                 "end": chunk["startMs"] / 1000 + item.end_time,
                                 "text": item.text})
    else:
        for chunk, text in recognized:
            segments.append({"start": chunk["startMs"] / 1000, "end": chunk["startMs"] / 1000 + 0.01,
                             "text": text})
    return segments


def funasr(model_dir, chunks):
    from funasr import AutoModel
    import torch
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; Fun-ASR Nano requires an NVIDIA CUDA PyTorch runtime")
    model = AutoModel(model=model_dir, trust_remote_code=True, hub="ms", device="cuda:0",
        vad_model="fsmn-vad", vad_kwargs={"max_single_segment_time": 30000})
    segments = []
    for chunk in chunks:
        result = model.generate(input=chunk["path"], language="中文", itn=True, batch_size=1)[0]
        segments.extend({"start": (chunk["startMs"] + item.get("start", 0)) / 1000,
                         "end": (chunk["startMs"] + item.get("end", 0)) / 1000,
                         "text": item.get("sentence", "")} for item in result.get("sentence_info", []))
    return segments


def main():
    if sys.argv[1] == "--check":
        check(sys.argv[2]); return
    request = json.load(open(sys.argv[1], encoding="utf-8"))
    output = qwen(request["modelDirectory"], request.get("forcedAlignerDirectory", ""), request["chunks"]) if request["provider"] == "qwen3" else funasr(request["modelDirectory"], request["chunks"])
    print(json.dumps({"segments": output}, ensure_ascii=False))


if __name__ == "__main__":
    main()
