"""L3 emotion2vec+ Base：仅本地文件、单批次识别，退出即释放模型。"""
import contextlib
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import socket
import sys
import time
import traceback
from types import SimpleNamespace

sys.dont_write_bytecode = True
LABELS = ['angry', 'disgusted', 'fearful', 'happy', 'neutral', 'other', 'sad', 'surprised', 'unknown']
WEIGHT_SHA = '60710b5aae1dbe69bdac8920028fb05882d4314fd09031922b4b61ee9e7aadbd'


def digest(path):
    with open(path, 'rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def prediction(scores):
    if len(scores) != 9 or any(not math.isfinite(x) or x < 0 or x > 1 for x in scores) or abs(sum(scores) - 1) > 1e-4:
        raise ValueError('情绪分数不是有效的九类概率分布。')
    order = sorted(range(9), key=lambda i: scores[i], reverse=True)
    top, second = order[:2]
    # 暂定审核策略，不将模型 softmax 分数表述为校准后的准确率。
    return {'label': LABELS[top], 'scores': scores, 'score': scores[top],
            'margin': scores[top] - scores[second], 'requiresReview': scores[top] < 0.6
            or scores[top] - scores[second] < 0.15 or LABELS[top] in ('unknown', 'other'),
            'policy': 'review-v1-score0.6-margin0.15', 'reviewed': False}


def blocked(*args, **kwargs):
    raise RuntimeError('L3 严格离线：禁止网络访问和自动下载。')


def run(request):
    batch_started = time.monotonic()
    root = Path(request['modelDirectory']).resolve(strict=True)
    if not root.is_dir():
        raise ValueError('请选择本地模型目录。')
    jobs = request['jobs']
    if not isinstance(jobs, list) or not 1 <= len(jobs) <= 100:
        raise ValueError('识别批次需要 1–100 个参考音频。')
    manifest = {name: digest(root / name) for name in ('model.pt', 'config.yaml', 'tokens.txt', 'configuration.json')}
    if manifest['model.pt'] != WEIGHT_SHA:
        raise ValueError('当前仅支持已核验的 emotion2vec+ Base 权重，SHA-256 不匹配。')
    import torch
    import soundfile as sf
    import numpy as np
    import yaml
    from scipy.signal import resample_poly
    # 显式导入，避免 FunASR 顶层批量导入吞掉模型依赖错误。
    from funasr.models.emotion2vec.model import Emotion2vec
    if not torch.cuda.is_available():
        raise RuntimeError('当前独立环境 CUDA 不可用；未自动改用其它环境。')
    config = yaml.safe_load((root / 'config.yaml').read_text(encoding='utf-8'))
    tokens = (root / 'tokens.txt').read_text(encoding='utf-8-sig').splitlines()
    if [x.rsplit('/', 1)[-1] for x in tokens] != LABELS[:-1] + ['<unk>']:
        raise ValueError('模型词表顺序不匹配。')
    if config['model'] != 'Emotion2vec' or config['model_conf']['depth'] != 8 or config['model_conf']['embed_dim'] != 768:
        raise ValueError('模型配置不是当前已核验的 Base 架构。')
    if config['model_conf']['modalities']['audio']['feature_encoder_spec'] != '[(512, 10, 5)] + [(512, 3, 2)] * 4 + [(512,2,2)] + [(512,2,2)]':
        raise ValueError('模型卷积配置不在已核验范围，拒绝上游 eval。')
    # PyYAML 把官方文件中的 1e-05 读为字符串；只规范化已知的 epsilon 字段。
    config['model_conf']['norm_eps'] = float(config['model_conf']['norm_eps'])
    if not 0 < config['model_conf']['norm_eps'] < 0.01:
        raise ValueError('模型归一化 epsilon 无效。')
    model = Emotion2vec(model_conf=config['model_conf'], vocab_size=9)
    # 分类权重不包含预训练重建 decoder；使用上游提供的方法移除未参与 extract_features 的模块。
    model.modality_encoders['AUDIO'].remove_pretraining_modules()
    state = torch.load(root / 'model.pt', map_location='cpu', weights_only=True)['model']
    mapped = {}
    for name, value in model.state_dict().items():
        source = name if name in state else 'd2v_model.' + name
        if source not in state or state[source].shape != value.shape:
            raise ValueError('模型参数缺失或形状不符：' + name)
        mapped[name] = state[source]
    model.load_state_dict(mapped, strict=True)
    parameter_count = sum(x.numel() for x in model.parameters())
    del mapped, state
    model.eval().to('cuda')
    torch.cuda.reset_peak_memory_stats()
    load_seconds = time.monotonic() - batch_started
    packages = {name: importlib.metadata.version(name) for name in ('torch', 'torchaudio', 'funasr', 'modelscope', 'numpy')}
    results = []
    version = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    for job in jobs:
        path = Path(job['path']).resolve(strict=True)
        before = digest(path)
        if before != job['sha256']:
            raise ValueError('识别前音频指纹不一致。')
        info = sf.info(path)
        if not 0.25 <= info.duration <= 30 or info.channels > 8 or info.samplerate < 8000 or info.samplerate > 192000:
            raise ValueError('情绪参考需为 0.25–30 秒、最多 8 声道、8–192kHz 的可解码音频。')
        audio, rate = sf.read(path, dtype='float32', always_2d=True)
        if not np.isfinite(audio).all() or np.max(np.abs(audio)) == 0:
            raise ValueError('参考含非有限值或全静音。')
        audio = audio.mean(axis=1)
        divisor = math.gcd(rate, 16000)
        audio = resample_poly(audio, 16000 // divisor, rate // divisor).astype(np.float32)
        started = time.monotonic()
        with torch.inference_mode():
            native, _ = model.inference([audio], key=[job['id']], tokenizer=SimpleNamespace(token_list=tokens),
                device='cuda', fs=16000, granularity='utterance', extract_embedding=False)
            if len(native) != 1 or native[0]['labels'] != tokens:
                raise ValueError('FunASR 原生输出标签不匹配。')
            scores = native[0]['scores']
        result = prediction(scores)
        result.update({'id': job['id'], 'assetSha256': before, 'model': 'emotion2vec_plus_base',
                       'modelVersion': version, 'weightSha256': manifest['model.pt'],
                       'seconds': time.monotonic() - started, 'inputSeconds': info.duration, 'runtime': packages})
        if digest(path) != before:
            raise ValueError('识别期间参考文件发生变化。')
        results.append(result)
        del native
    if any(digest(root / name) != value for name, value in manifest.items()):
        raise ValueError('识别期间模型文件发生变化。')
    return {'type': 'emotion', 'results': results, 'manifest': manifest, 'parameters': parameter_count,
            'loadSeconds': load_seconds, 'batchSeconds': time.monotonic() - batch_started,
            'loadedParameters': len(model.state_dict()), 'cudaDevice': torch.cuda.get_device_name(0),
            'peakAllocatedBytes': torch.cuda.max_memory_allocated(), 'peakReservedBytes': torch.cuda.max_memory_reserved(),
            'packages': packages}


def main():
    for stream in (sys.stdout, sys.stderr, sys.stdin):
        stream.reconfigure(encoding='utf-8')
    for name in ('HF_HUB_OFFLINE', 'TRANSFORMERS_OFFLINE', 'MODELSCOPE_OFFLINE', 'HF_HUB_DISABLE_TELEMETRY'):
        os.environ[name] = '1'
    socket.socket.connect = blocked
    socket.socket.connect_ex = blocked
    socket.create_connection = blocked
    try:
        request = json.loads(sys.stdin.readline(1024 * 1024))
        # 保持 stdout 为单一结构化结果，依赖诊断写 stderr。
        with contextlib.redirect_stdout(sys.stderr):
            result = run(request)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
    except Exception:
        traceback.print_exc(file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
