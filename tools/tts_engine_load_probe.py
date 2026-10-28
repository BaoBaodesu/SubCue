"""L1 显式真实加载验证：严格离线、独立进程、仅输出诊断，不改权重。"""
import argparse
import gc
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import time
import traceback

sys.dont_write_bytecode = True
from tts_engine_preflight import inspect


def blocked(*args, **kwargs):
    raise RuntimeError('L1 验证禁止网络访问和自动下载。')


def digest(path):
    result = hashlib.sha256()
    with open(path, 'rb') as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', choices=('index', 'gpt'), required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.repository).resolve()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.chdir(output)
    for name in ('HF_HUB_OFFLINE', 'TRANSFORMERS_OFFLINE', 'HF_HUB_DISABLE_TELEMETRY'):
        os.environ[name] = '1'
    for name in ('HF_HOME', 'TORCH_HOME', 'NUMBA_CACHE_DIR', 'CUDA_CACHE_PATH', 'MPLCONFIGDIR', 'TEMP', 'TMP'):
        os.environ[name] = str(output / 'cache')
    (output / 'cache').mkdir(exist_ok=True)
    socket.socket.connect = blocked
    socket.socket.connect_ex = blocked
    socket.create_connection = blocked
    report = inspect(args.engine, root)
    before = {name: digest(value['path']) for name, value in report['manifest'].items()}
    report['sha256Before'] = before
    started = time.monotonic()
    model = None
    try:
        if not report['filesAndPackagesPresent']:
            raise RuntimeError('预检不通过，未加载：' + str(report['missingFiles']))
        import torch
        if not torch.cuda.is_available():
            raise RuntimeError('CUDA 不可用，拒绝自动回退。')
        report['device'] = torch.cuda.get_device_name(0)
        report['deviceBytes'] = torch.cuda.get_device_properties(0).total_memory
        torch.cuda.reset_peak_memory_stats()
        sys.path.insert(0, str(root))
        if args.engine == 'index':
            import yaml
            cfg = yaml.safe_load((root / 'checkpoints/config.yaml').read_text(encoding='utf-8'))
            if str(cfg.get('version')) != '2.5':
                raise RuntimeError('Index 配置不是 2.5，拒绝加载。')
            import indextts.utils.model_download as downloads
            downloads.ensure_models_available = lambda *a, **k: report['auxiliaryPaths']
            downloads.snapshot_download = blocked
            downloads._download_single_file = blocked
            from indextts.infer_v2_5 import IndexTTS2
            model = IndexTTS2(cfg_path=str(root / 'checkpoints/config.yaml'), model_dir=str(root / 'checkpoints'),
                device='cuda:0', use_bf16=True, use_cuda_kernel=False, use_deepspeed=False,
                use_accel=False, use_torch_compile=False, use_qwen_emo=False)
        else:
            sys.path.insert(0, str(root / 'GPT_SoVITS'))
            sys.path.insert(0, str(root / 'GPT_SoVITS/eres2net'))
            # 上游 i18n 的 relpath 不支持跨盘；只在导入此只读模块时使用源码目录。
            os.chdir(root)
            try:
                from tools.i18n import i18n
                i18n.I18N_JSON_DIR = str(root / 'tools/i18n/locale')
            finally:
                os.chdir(output)
            import sv
            sv.sv_path = str(root / 'GPT_SoVITS/pretrained_models/sv/pretrained_eres2netv2w24s4ep4.ckpt')
            from TTS_infer_pack.TTS import TTS
            base = root / 'GPT_SoVITS/pretrained_models'
            model = TTS({'custom': {'version': 'v2ProPlus', 'device': 'cuda:0', 'is_half': True,
                't2s_weights_path': str(base / 's1v3.ckpt'), 'vits_weights_path': str(base / 'v2Pro/s2Gv2ProPlus.pth'),
                'bert_base_path': report['auxiliaryPaths']['bert'], 'cnhuhbert_base_path': report['auxiliaryPaths']['hubert']}})
            if model.configs.version != 'v2ProPlus':
                raise RuntimeError('实际加载型号与 v2ProPlus 不匹配。')
        torch.cuda.synchronize()
        report.update(weightsLoaded=True, runtimeVerified=True, loadMs=int((time.monotonic() - started) * 1000),
            peakAllocatedBytes=torch.cuda.max_memory_allocated(), peakReservedBytes=torch.cuda.max_memory_reserved())
    except Exception as error:
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
        print(report['traceback'], flush=True)
    finally:
        model = None
        gc.collect()
        if 'torch' in sys.modules:
            torch = sys.modules['torch']
            if torch.cuda.is_available():
                report.setdefault('peakAllocatedBytes', torch.cuda.max_memory_allocated())
                report.setdefault('peakReservedBytes', torch.cuda.max_memory_reserved())
                torch.cuda.empty_cache()
                report['allocatedAfterReleaseBytes'] = torch.cuda.memory_allocated()
        report['sha256After'] = {name: digest(value['path']) for name, value in report['manifest'].items()}
        report['filesUnchanged'] = report['sha256After'] == before
        (output / 'load-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if report.get('runtimeVerified') and report['filesUnchanged'] else 2


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
