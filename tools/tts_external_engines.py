"""两个固定后续本地引擎；复用主 Worker 协议与严格离线策略。"""
import contextlib
import gc
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import traceback

from tts_engine_preflight import inspect


# GPT-SoVITS 的 text_lang／prompt_lang 取值；参考语言与目标语言分别映射，参考转写保持原语言。
GPT_LANGUAGE_CODES = {'Chinese': 'zh', 'English': 'en', 'Japanese': 'ja'}


def check(provider, repository, languages=None):
    from tts_python_worker import digest
    engine = {'index-local': 'index', 'gpt-local': 'gpt'}[provider]
    # 未指定语言时按中文基线预检，保持既有中文安装的通过条件；指定语言时只要求这些语言的前端资源。
    report = inspect(engine, repository) if languages is None else inspect(engine, repository, languages=languages)
    if not report['filesAndPackagesPresent']:
        raise ValueError('离线预检不通过：' + json.dumps({'missing': report['missingFiles'], 'configuration': report['configurationErrors'], 'packages': report['packages']}, ensure_ascii=False))
    if report['missingGenerationResources']:
        raise ValueError('所选目标／参考语言所需的离线生成资源缺失：' + json.dumps({'languages': report.get('requestedLanguages', []), 'missing': report['missingGenerationResources']}, ensure_ascii=False))
    for value in report['manifest'].values():
        value['sha256'] = digest(value['path'])
    report['profile'] = {'provider': provider, 'precision': 'bf16' if engine == 'index' else 'fp16',
                         'emotionText': False, 'acceleration': False, 'adapterVersion': 2}
    if engine == 'gpt':
        report['profile'].update(languageDetector='full-local', adapterVersion=3, cudaGraph=False, flashAttention=False,
                                 supportedTextLanguages=report.get('supportedTextLanguages', []))
        import onnxruntime
        report['auxiliaryProviders'] = ['CUDAExecutionProvider', 'CPUExecutionProvider'] if 'CUDAExecutionProvider' in onnxruntime.get_available_providers() else ['CPUExecutionProvider']
        report['profile'].update(g2pwProviders=report['auxiliaryProviders'], onnxRuntimeVersion=onnxruntime.__version__)
    identity = {name: {'size': value['size'], 'sha256': value['sha256']} for name, value in report['manifest'].items()}
    report['modelVersion'] = 'sha256:' + hashlib.sha256(json.dumps({'manifest': identity, 'packages': report['packages'], 'profile': report['profile']}, sort_keys=True).encode()).hexdigest()
    return report


def load(provider, root, output, checked):
    import torch
    from tts_python_worker import deny_network
    if not torch.cuda.is_available():
        raise RuntimeError('CUDA 不可用，不自动回退。')
    sys.path.insert(0, str(root))
    if provider == 'index-local':
        import indextts.utils.model_download as downloads
        downloads.ensure_models_available = lambda *a, **k: checked['auxiliaryPaths']
        downloads.snapshot_download = deny_network
        downloads._download_single_file = deny_network
        from indextts.infer_v2_5 import IndexTTS2
        return IndexTTS2(cfg_path=str(root / 'checkpoints/config.yaml'), model_dir=str(root / 'checkpoints'), device='cuda:0',
                         use_bf16=True, use_cuda_kernel=False, use_deepspeed=False, use_accel=False, use_torch_compile=False, use_qwen_emo=False)
    sys.path.insert(0, str(root / 'GPT_SoVITS'))
    sys.path.insert(0, str(root / 'GPT_SoVITS/eres2net'))
    os.chdir(root)
    try:
        from tools.i18n import i18n
        i18n.I18N_JSON_DIR = str(root / 'tools/i18n/locale')
    finally:
        os.chdir(output)
    import sv
    base = root / 'GPT_SoVITS/pretrained_models'
    sv.sv_path = str(base / 'sv/pretrained_eres2netv2w24s4ep4.ckpt')
    os.environ['bert_path'] = checked['auxiliaryPaths']['bert']
    import text.g2pw.onnx_api as g2pw
    def local_g2pw(directory):
        path = root / 'GPT_SoVITS/text/G2PWModel'
        if not path.is_dir():
            raise FileNotFoundError('缺少 G2PW 本地目录，禁止下载。')
        return str(path)
    g2pw.download_and_decompress = local_g2pw
    from TTS_infer_pack.TTS import TTS
    import fast_langdetect
    # 显式加载已验证的大模型，禁止 auto 模式在内存或资源错误时悄悄改用 lite。
    fast_langdetect.infer._default_detector = fast_langdetect.infer.LangDetector(fast_langdetect.infer.LangDetectConfig(
        cache_dir=str(output / '.worker-cache'), custom_model_path=str(base / 'fast_langdetect/lid.176.bin'), model='full'))
    fast_langdetect.infer._default_detector.detect('这是一段中文离线模型验证文本。', model='full')
    model = TTS({'custom': {'version': 'v2ProPlus', 'device': 'cuda:0', 'is_half': True,
        't2s_weights_path': str(base / 's1v3.ckpt'), 'vits_weights_path': str(base / 'v2Pro/s2Gv2ProPlus.pth'),
        'bert_base_path': checked['auxiliaryPaths']['bert'], 'cnhuhbert_base_path': checked['auxiliaryPaths']['hubert']}})
    if model.configs.version != 'v2ProPlus':
        raise RuntimeError('实际权重版本与 v2ProPlus 不匹配。')
    return model


def batch(request):
    from tts_python_worker import validate_jobs, publish, emit
    import torch
    import numpy as np
    import soundfile as sf
    provider = request['provider']
    # GPT 现在允许中英日目标与参考语言；Index 保持原有的中英日目标语言。
    validate_jobs(request.get('jobs'), False, transcript_required=provider != 'index-local', index_controls=provider == 'index-local',
                  languages=('Chinese', 'English', 'Japanese'), gpt_controls=provider == 'gpt-local')
    output = Path(request['outputDirectory'])
    if not output.is_absolute() or not output.is_dir():
        raise ValueError('输出目录必须由任务所有者创建。')
    root = Path(request['modelDirectory'])
    # 预检只要求本批实际用到的目标／参考语言资源；模型身份不受语言选择影响。
    requested_languages = tuple(sorted({job['language'] for job in request['jobs']} |
                                       {job.get('referenceLanguage', 'Chinese') for job in request['jobs']}))
    checked = check(provider, root, requested_languages) if provider == 'gpt-local' else check(provider, root)
    if any(job['modelVersion'] != checked['modelVersion'] for job in request['jobs']):
        raise ValueError('模型或环境版本变化，请重新预检。')
    os.chdir(output)
    cache = output / '.worker-cache'
    cache.mkdir(exist_ok=True)
    for name in ('HF_HOME', 'TORCH_HOME', 'NUMBA_CACHE_DIR', 'CUDA_CACHE_PATH', 'MPLCONFIGDIR', 'TEMP', 'TMP'):
        os.environ[name] = str(cache)
    model = None
    try:
        with contextlib.redirect_stdout(sys.stderr):
            model = load(provider, root, output, checked)
            torch.cuda.reset_peak_memory_stats()
        for job in request['jobs']:
            started = time.monotonic()
            emit({'type': 'job_started', 'jobId': job['jobId'], 'inputHash': job['inputHash'], 'mock': False})
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    if 'seed' in job.get('parameters', {}):
                        torch.manual_seed(job['parameters']['seed'])
                    if provider == 'index-local':
                        parameters = job.get('parameters', {})
                        vector = None
                        if 'emotion' in parameters:
                            vector = [0.0] * 8
                            vector[('happy', 'angry', 'sad', 'afraid', 'disgusted', 'depressed', 'surprised', 'calm').index(parameters['emotion'])] = parameters['emotionStrength']
                            vector = model.normalize_emo_vec(vector)
                        generated = model.infer(spk_audio_prompt=job['referencePath'], text=job['spokenText'], output_path=None,
                                                lang={'Chinese': 'zh', 'English': 'en', 'Japanese': 'ja'}[job['language']], interval_silence=0, stream_return=False, emo_vector=vector,
                                                use_random=False, duration_factor=parameters.get('durationFactor', 1.0))
                    else:
                        # 目标语言与参考语言分别映射；参考转写必须保持参考音频原语言，不翻译。
                        text_lang = GPT_LANGUAGE_CODES[job['language']]
                        prompt_lang = GPT_LANGUAGE_CODES[job.get('referenceLanguage', 'Chinese')]
                        generated = list(model.run({'text': job['spokenText'], 'text_lang': text_lang, 'ref_audio_path': job['referencePath'],
                            'prompt_text': job['referenceText'], 'prompt_lang': prompt_lang, 'text_split_method': 'cut0', 'batch_size': 1,
                            'parallel_infer': False, 'use_cuda_graph': False, 'use_flash_attention': False,
                            'speed_factor': job.get('parameters', {}).get('speedFactor', 1.0),
                            'split_bucket': False, 'fragment_interval': 0, 'seed': job.get('parameters', {}).get('seed', -1)}))
                        if getattr(model, 't2s_accel', None) is not None:
                            raise RuntimeError('GPT 实际启用了未获预检允许的语义加速，拒绝提交结果。')
                        if len(generated) != 1:
                            raise RuntimeError('句级非流式结果数量异常。')
                        generated = generated[0]
                if generated is None:
                    raise RuntimeError('引擎没有返回音频。')
                rate, samples = generated
                samples = np.asarray(samples)
                samples = np.squeeze(samples)
                if samples.ndim != 1 or not samples.size or not np.isfinite(samples).all():
                    raise RuntimeError('音频为空、非单声道或非有限。')
                if samples.dtype.kind in ('i', 'u'):
                    samples = samples.astype(np.float32) / float(np.iinfo(samples.dtype).max + 1)
                def writer(path):
                    sf.write(path, samples, rate, format='WAV', subtype='FLOAT')
                    return {'sampleRate': int(rate), 'channels': 1, 'sampleCount': int(samples.size)}
                path, specs = publish(output, job, writer)
                auxiliary = {}
                if provider == 'gpt-local':
                    import text.chinese2 as chinese
                    import fast_langdetect
                    auxiliary = {'textLang': text_lang, 'promptLang': prompt_lang,
                                 'g2pwProviders': chinese.g2pw._converter._g2pw.session_g2pw.get_providers(),
                                 'languageDetectorProfiles': list(fast_langdetect.infer._default_detector._models),
                                 'loadedVersion': model.configs.version,
                                 'accelerationActive': getattr(model, 't2s_accel', None) is not None,
                                 'semanticTokenCount': int(model.t2s_model.model.ar_audio_embedding.weight.shape[0]),
                                 'maxSeconds': model.configs.max_sec}
                    if auxiliary['g2pwProviders'] != checked['auxiliaryProviders']:
                        raise RuntimeError('G2PW 实际后端与预检不匹配，拒绝隐式后端降级。')
                emit({'type': 'result', 'jobId': job['jobId'], 'inputHash': job['inputHash'], 'audioPath': str(path), **specs,
                    'mock': False, 'elapsedMs': int((time.monotonic() - started) * 1000), 'usage': {'state': 'unknown'},
                    'diagnostics': {'modelVersion': checked['modelVersion'], 'packages': checked['packages'], 'provider': provider,
                        'language': job['language'], 'referenceLanguage': job.get('referenceLanguage', 'Chinese'),
                        'effectiveParameters': job.get('parameters', {}),
                        **auxiliary, 'device': torch.cuda.get_device_name(0), 'peakAllocatedBytes': torch.cuda.max_memory_allocated(),
                        'peakReservedBytes': torch.cuda.max_memory_reserved()}})
                writer = None
                del generated, samples
            except Exception as error:
                emit({'type': 'error', 'jobId': job['jobId'], 'mock': False,
                      'code': 'oom' if 'out of memory' in str(error).lower() else 'generation_failed', 'message': str(error), 'traceback': traceback.format_exc()})
                # 不在疑似损坏或 OOM 的模型状态中继续整个批次。
                break
    finally:
        model = None
        gc.collect()
        torch.cuda.empty_cache()
    emit({'type': 'batch_finished', 'mock': False})
