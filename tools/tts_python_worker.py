"""SubCue 句级 TTS 批次协议；离线运行，不安装依赖或注册云音色。"""
import contextlib
import hashlib
import importlib.metadata
import json
import math
import re
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import uuid
import wave
import socket
import gc
import traceback

os.environ['HF_HUB_OFFLINE'] = '1'
os.environ['TRANSFORMERS_OFFLINE'] = '1'
os.environ['HF_HUB_DISABLE_TELEMETRY'] = '1'
sys.dont_write_bytecode = True


def deny_network(*args, **kwargs):
    raise RuntimeError('本地 TTS 严格离线，不允许联网或自动下载依赖。')


socket.socket.connect = deny_network
socket.socket.connect_ex = deny_network
socket.create_connection = deny_network
for stream in (sys.stdin, sys.stdout, sys.stderr):
    if hasattr(stream, 'reconfigure'):
        stream.reconfigure(encoding='utf-8')


def emit(event):
    print(json.dumps(event, ensure_ascii=False, allow_nan=False), flush=True)


def digest(path):
    result = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def preflight(directory):
    root = Path(directory)
    if not root.is_absolute() or not root.is_dir():
        raise ValueError('请配置本地 Qwen Base 模型目录，不能使用远程模型 ID。')
    required = ('config.json', 'generation_config.json', 'tokenizer_config.json', 'vocab.json', 'merges.txt',
                'preprocessor_config.json', 'model.safetensors', 'speech_tokenizer/config.json',
                'speech_tokenizer/preprocessor_config.json', 'speech_tokenizer/model.safetensors')
    manifest = {}
    for file in required:
        if not (root / file).is_file() or (root / file).stat().st_size == 0:
            raise ValueError('缺少本地模型文件：' + file)
        manifest[file] = {'size': (root / file).stat().st_size}
    config = json.loads((root / 'config.json').read_text(encoding='utf-8'))
    if config.get('tts_model_type') != 'base' or config.get('tokenizer_type') != 'qwen3_tts_tokenizer_12hz' or config.get('tts_model_size') != '0b6':
        raise ValueError('首版仅接入 Qwen 12Hz Base，不能混用其它型号能力。')
    language_ids = config.get('talker_config', {}).get('codec_language_id', {})
    if any(language not in language_ids for language in ('chinese', 'english', 'japanese')):
        raise ValueError('实际权重配置缺少中英日语言标识，不能按系列宣传启用。')
    packages = {}
    for package in ('qwen-tts', 'torch', 'torchaudio', 'transformers', 'soundfile', 'librosa', 'huggingface-hub', 'tokenizers', 'safetensors'):
        packages[package] = importlib.metadata.version(package)
    from packaging.requirements import Requirement
    incompatible = []
    for owner in ('qwen-tts', 'transformers', 'torch', 'torchaudio'):
        for declared in importlib.metadata.requires(owner) or []:
            requirement = Requirement(declared)
            if requirement.marker and not requirement.marker.evaluate({'extra': ''}):
                continue
            try:
                actual = importlib.metadata.version(requirement.name)
            except importlib.metadata.PackageNotFoundError:
                actual = 'missing'
            if actual == 'missing' or (requirement.specifier and not requirement.specifier.contains(actual, prereleases=True)):
                incompatible.append({'owner': owner, 'requirement': declared, 'actual': actual})
    if incompatible:
        raise ValueError('独立环境依赖声明不匹配，不加载模型：' + json.dumps(incompatible, ensure_ascii=False))
    for file in required:
        manifest[file]['sha256'] = digest(root / file)
    profile = {'attention': 'sdpa', 'talkerPrecision': 'bf16-if-supported-else-fp16', 'tokenizerPrecision': 'float32', 'nonStreamingMode': False,
               'chineseTalkerDoSample': False, 'chineseRepetitionPenalty': 1.15}
    identity = hashlib.sha256(json.dumps({'files': manifest, 'packages': packages, 'profile': profile}, sort_keys=True).encode()).hexdigest()
    return {'modelType': 'base', 'packages': packages, 'weightsLoaded': False, 'manifest': manifest,
            'modelVersion': 'sha256:' + identity, 'offline': True, 'inferenceProfile': profile, 'languages': ['Chinese', 'English', 'Japanese']}


def validate_jobs(jobs, mock, transcript_required=True, index_controls=False, languages=('Chinese',), gpt_controls=False):
    if not isinstance(jobs, list) or not jobs or len(jobs) > 500:
        raise ValueError('批次需要 1–500 个句级任务。')
    ids = set()
    for job in jobs:
        identity = str(uuid.UUID(job['jobId']))
        if uuid.UUID(identity).int == 0 or identity != job['jobId'] or identity in ids:
            raise ValueError('任务 UUID 重复或格式不规范。')
        ids.add(identity)
        if mock and job.get('simulate', '') not in ('', 'crash', 'oom', 'delay', 'invalid_audio', 'silence'):
            raise ValueError('未知 Mock 故障类型。')
        if not isinstance(job.get('spokenText'), str) or not job['spokenText'].strip() or len(job['spokenText']) > 4096:
            raise ValueError('句级朗读文本为空或超过 4096 字符，请重新分段。')
        if not index_controls and re.search(r'<[^<>|]+\|[^<>|]+>', job['spokenText']):
            raise ValueError('当前引擎不支持 Index 发音标注，请修改朗读文本。')
        if len(job.get('inputHash', '')) != 64 or len(bytes.fromhex(job['inputHash'])) != 32:
            raise ValueError('生成输入指纹无效。')
        if job.get('language', 'Chinese') not in languages or (not mock and job.get('referenceLanguage', 'Chinese') not in languages):
            raise ValueError('当前引擎尚未接入所选目标／参考语言，不自动猜测或切换引擎。')
        parameters = job.get('parameters', {})
        if not isinstance(parameters, dict) or set(parameters) - ({'seed', 'durationFactor', 'emotion', 'emotionStrength'} if index_controls else {'seed', 'speedFactor'} if gpt_controls else {'seed'}):
            raise ValueError('当前引擎不支持这些参数，不能假定已生效。')
        if 'seed' in parameters and (type(parameters['seed']) is not int or not 0 <= parameters['seed'] <= 0x7fffffff):
            raise ValueError('seed 必须是 0–2147483647 的整数。')
        if 'durationFactor' in parameters and (type(parameters['durationFactor']) not in (int, float) or not 0.5 <= parameters['durationFactor'] <= 2):
            raise ValueError('Index 时长倍率必须为 0.5～2 的有限数值。')
        if 'speedFactor' in parameters and (type(parameters['speedFactor']) not in (int, float) or not 0.5 <= parameters['speedFactor'] <= 2):
            raise ValueError('GPT 语速倍率必须为 0.5～2 的有限数值。')
        if ('emotion' in parameters) != ('emotionStrength' in parameters):
            raise ValueError('情绪类别与强度必须同时提供。')
        if 'emotion' in parameters and (parameters['emotion'] not in ('happy', 'angry', 'sad', 'afraid', 'disgusted', 'depressed', 'surprised', 'calm')
            or type(parameters['emotionStrength']) not in (int, float) or not 0 < parameters['emotionStrength'] <= 1):
            raise ValueError('Index 情绪类别或强度无效。')
        if not mock:
            reference = Path(job.get('referencePath', ''))
            if not reference.is_absolute() or not reference.is_file() or (transcript_required and not job.get('referenceText', '').strip()):
                raise ValueError('本地克隆需要参考文件和已审核转写，不接受 URL。')
            if digest(reference) != job.get('referenceSha256'):
                raise ValueError('参考音频内容变化，未生成。')


def publish(root, job, writer):
    final = root / (job['jobId'] + '.wav')
    descriptor, temporary = tempfile.mkstemp(prefix='.tts-', suffix='.part', dir=root)
    os.close(descriptor)
    try:
        specification = writer(temporary)
        # 创建硬链接保证原子发布且不覆盖已有任务结果。
        os.link(temporary, final)
        return final, specification
    finally:
        os.unlink(temporary)


def mock_audio(path, simulation):
    if simulation == 'invalid_audio':
        Path(path).write_bytes(b'clearly marked invalid mock audio')
        return {'sampleRate': 0, 'channels': 0, 'sampleCount': 0}
    rate, count = 24000, 12000
    with wave.open(path, 'wb') as output:
        output.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
        for begin in range(0, count, 1024):
            block = [0 if simulation == 'silence' else int(2000 * math.sin(2 * math.pi * 440 * index / rate))
                     for index in range(begin, min(begin + 1024, count))]
            output.writeframesraw(struct.pack('<' + 'h' * len(block), *block))
    return {'sampleRate': rate, 'channels': 1, 'sampleCount': count}


def batch(request):
    if request.get('provider') in ('index-local', 'gpt-local'):
        from tts_external_engines import batch as external_batch
        external_batch(request)
        return
    mock = request.get('provider') == 'mock'
    if not mock and request.get('provider') != 'qwen-local':
        raise ValueError('不支持的本地 Provider。')
    validate_jobs(request.get('jobs'), mock, languages=('Chinese',) if mock else ('Chinese', 'English', 'Japanese'))
    root = Path(request['outputDirectory'])
    if not root.is_absolute() or not root.is_dir():
        raise ValueError('输出目录必须由主程序预先创建。')
    model = None
    prompts = {}
    try:
        if not mock:
            checked = preflight(request['modelDirectory'])
            if any(job.get('modelVersion') != checked['modelVersion'] for job in request['jobs']):
                raise ValueError('模型或运行环境版本已变化，请重新预检并确认生成输入。')
            cache = root / '.worker-cache'
            cache.mkdir(exist_ok=True)
            os.environ['NUMBA_CACHE_DIR'] = str(cache)
            os.environ['HF_HOME'] = str(cache / 'hf')
            os.environ['CUDA_CACHE_PATH'] = str(cache / 'cuda')
            with contextlib.redirect_stdout(sys.stderr):
                import torch
                import numpy as np
                import soundfile as sf
                from qwen_tts import Qwen3TTSModel
                if not torch.cuda.is_available():
                    raise RuntimeError('CUDA 不可用，不自动切换引擎。')
                model = Qwen3TTSModel.from_pretrained(request['modelDirectory'], device_map='cuda:0',
                    dtype=torch.bfloat16 if torch.cuda.is_bf16_supported() else torch.float16,
                    attn_implementation='sdpa', local_files_only=True)
                # 声学编码／解码保留浮点精度，避免最终波形被 BF16 量化；主合成模型仍使用半精度。
                model.model.speech_tokenizer.model.float()
                torch.cuda.reset_peak_memory_stats()
        for job in request['jobs']:
            started = time.monotonic()
            emit({'type': 'job_started', 'jobId': job['jobId'], 'inputHash': job['inputHash'], 'mock': mock})
            try:
                if mock:
                    simulation = job.get('simulate', '')
                    if simulation == 'crash':
                        os._exit(18)
                    if simulation == 'oom':
                        raise RuntimeError('Mock CUDA out of memory')
                    if simulation == 'delay':
                        time.sleep(30)
                    writer = lambda path: mock_audio(path, simulation)
                else:
                    with contextlib.redirect_stdout(sys.stderr):
                        key = (job['referenceSha256'], job['referenceText'], job.get('referenceLanguage', 'Chinese'))
                        if key not in prompts:
                            if len(prompts) >= 4:
                                del prompts[next(iter(prompts))]
                            reference, reference_rate = sf.read(job['referencePath'], dtype='float32')
                            if reference.ndim != 1 or not np.isfinite(reference).all():
                                raise ValueError('项目参考必须为可解码的有限单声道音频。')
                            prompts[key] = model.create_voice_clone_prompt(ref_audio=(reference, reference_rate), ref_text=job['referenceText'])
                            del reference
                        if 'seed' in job.get('parameters', {}):
                            torch.manual_seed(job['parameters']['seed'])
                        # 中文采用整轨试听通过的主声码参数，其它语言沿用原模型设置。
                        audio, rate = model.generate_voice_clone(text=job['spokenText'], language=job['language'], voice_clone_prompt=prompts[key],
                            non_streaming_mode=checked['inferenceProfile']['nonStreamingMode'],
                            **({'do_sample': checked['inferenceProfile']['chineseTalkerDoSample'],
                                'repetition_penalty': checked['inferenceProfile']['chineseRepetitionPenalty']} if job['language'] == 'Chinese' else {}))
                    samples = audio[0]
                    if samples.ndim != 1 or not samples.size or not np.isfinite(samples).all():
                        raise RuntimeError('生成结果为空、非单声道或包含非有限值。')
                    def writer(path):
                        sf.write(path, samples, rate, format='WAV', subtype='FLOAT')
                        return {'sampleRate': int(rate), 'channels': 1, 'sampleCount': int(samples.size)}
                path, specification = publish(root, job, writer)
                emit({'type': 'result', 'jobId': job['jobId'], 'inputHash': job['inputHash'], 'audioPath': str(path),
                      **specification, 'mock': mock, 'elapsedMs': int((time.monotonic() - started) * 1000),
                      'diagnostics': {} if mock else {'modelVersion': checked['modelVersion'], 'packages': checked['packages'],
                          'language': job['language'], 'referenceLanguage': job.get('referenceLanguage', 'Chinese'),
                          'device': torch.cuda.get_device_name(0), 'dtype': str(model.model.dtype),
                          'tokenizerDtype': str(model.model.speech_tokenizer.model.dtype), 'inferenceProfile': checked['inferenceProfile'],
                          'attention': 'sdpa', 'peakAllocatedBytes': torch.cuda.max_memory_allocated(),
                          'peakReservedBytes': torch.cuda.max_memory_reserved(), 'cloneConditionsCached': len(prompts)},
                      'usage': {'state': 'unknown'}})
            except Exception as error:
                emit({'type': 'error', 'jobId': job['jobId'], 'mock': mock,
                      'code': 'oom' if 'out of memory' in str(error).lower() else 'generation_failed', 'message': str(error), 'traceback': traceback.format_exc()})
                if not mock:
                    # 真实引擎异常后停止批次，不继续使用可能损坏或显存不足的模型。
                    break
            finally:
                if not mock:
                    writer = audio = samples = reference = None
    finally:
        if not mock:
            prompts.clear()
            model = None
            gc.collect()
            if 'torch' in locals():
                torch.cuda.empty_cache()
    emit({'type': 'batch_finished', 'mock': mock})


def main():
    try:
        line = sys.stdin.buffer.readline(2 * 1024 * 1024 + 1)
        if len(line) > 2 * 1024 * 1024:
            raise ValueError('批次请求超过 2MiB。')
        request = json.loads(line)
        if request.get('action') == 'check':
            external = request.get('provider') in ('index-local', 'gpt-local')
            if external:
                from tts_external_engines import check
                checked = check(request['provider'], request['modelDirectory'])
            else:
                checked = preflight(request['modelDirectory'])
            cache = Path(request['outputDirectory'])
            if not cache.is_absolute() or not cache.is_dir():
                raise ValueError('预检缓存目录必须由主程序创建。')
            os.environ['NUMBA_CACHE_DIR'] = str(cache)
            os.environ['HF_HOME'] = str(cache / 'hf')
            os.environ['CUDA_CACHE_PATH'] = str(cache / 'cuda')
            with contextlib.redirect_stdout(sys.stderr):
                import torch
                import soundfile
                import librosa
                if not external:
                    from qwen_tts import Qwen3TTSModel
                if not torch.cuda.is_available():
                    raise RuntimeError('CUDA 不可用，请检查独立环境与显卡驱动。')
            emit({'type': 'check', **checked, 'device': torch.cuda.get_device_name(0),
                  'deviceBytes': torch.cuda.get_device_properties(0).total_memory, 'cuda': torch.version.cuda})
        elif request.get('action') == 'batch':
            batch(request)
        else:
            raise ValueError('未知 Worker 操作。')
    except Exception as error:
        emit({'type': 'fatal', 'message': str(error), 'code': 'preflight_or_protocol_failed', 'traceback': traceback.format_exc()})
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
