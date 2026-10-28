"""后续 TTS 引擎的只读离线预检；不导入模型、不创建缓存、不下载。"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import sys
import zipfile


def inspect_gpt_language_resources(nltk_roots=None, japanese_directory=None):
    # 只读文件预检，不导入会自动下载／生成词典的文本模块，也不扩大语言能力。
    if nltk_roots is None:
        nltk_roots = [Path(p) for p in os.environ.get('NLTK_DATA', '').split(os.pathsep) if p]
        nltk_roots += [Path(sys.prefix) / 'nltk_data', Path(sys.prefix) / 'share/nltk_data',
                       Path(sys.prefix) / 'lib/nltk_data', Path.home() / 'nltk_data']
        if os.environ.get('APPDATA'):
            nltk_roots.append(Path(os.environ['APPDATA']) / 'nltk_data')
    found, missing = {}, []
    for relative, member in (
        ('taggers/averaged_perceptron_tagger.zip', 'averaged_perceptron_tagger/averaged_perceptron_tagger.pickle'),
        ('corpora/cmudict.zip', 'cmudict/cmudict'),
        *[('taggers/averaged_perceptron_tagger_eng/' + name, '') for name in (
            'averaged_perceptron_tagger_eng.classes.json', 'averaged_perceptron_tagger_eng.tagdict.json',
            'averaged_perceptron_tagger_eng.weights.json')]):
        for root in nltk_roots:
            path = Path(root) / relative
            try:
                if not path.is_file() or not path.stat().st_size:
                    continue
                if member:
                    with zipfile.ZipFile(path) as archive:
                        if archive.getinfo(member).file_size <= 0:
                            continue
                found[relative] = str(path)
                break
            except (OSError, zipfile.BadZipFile, KeyError):
                continue
        else:
            missing.append(relative)
    if japanese_directory is None:
        try:
            japanese_directory = os.environ.get('OPEN_JTALK_DICT_DIR') or str(
                importlib.metadata.distribution('pyopenjtalk').locate_file('pyopenjtalk/open_jtalk_dic_utf_8-1.11'))
        except importlib.metadata.PackageNotFoundError:
            japanese_directory = str(Path(sys.prefix) / 'Lib/site-packages/pyopenjtalk/open_jtalk_dic_utf_8-1.11')
    japanese_missing = [name for name in ('sys.dic', 'unk.dic', 'char.bin', 'matrix.bin', 'left-id.def',
        'right-id.def', 'rewrite.def', 'pos-id.def')
        if not (Path(japanese_directory) / name).is_file() or not (Path(japanese_directory) / name).stat().st_size]
    return {'English': {'filesPresent': not missing, 'missing': missing, 'paths': found},
            'Japanese': {'filesPresent': not japanese_missing, 'missing': japanese_missing, 'directory': str(japanese_directory)},
            'runtimeVerified': False, 'qualityVerified': False,
            'notes': '可选英日资源文件检查；不影响现有中文就绪状态，不代表文本加载／生成／音质通过。'}


def inspect(engine, repository, auxiliary=None):
    root = Path(repository)
    if not root.is_absolute() or not root.is_dir():
        raise ValueError('引擎仓库必须是存在的本地绝对目录。')
    aux = Path(auxiliary) if auxiliary else (
        root / 'hf_cache' if engine == 'index' else root / 'GPT_SoVITS' / 'pretrained_models')
    if not aux.is_absolute():
        raise ValueError('辅助模型目录必须是本地绝对路径。')
    if engine == 'index':
        files = {name: root / 'checkpoints' / name for name in (
            'config.yaml', 'gpt.pth', 'codec.pth', 's2mel.pth', 'wav2vec2bert_stats.pt', 'feat1.pt', 'feat2.pt',
            'multilingual_zh_ja_yue_char_del.tiktoken', 'pinyin.vocab')}
        files.update({name: aux / name for name in (
            'w2v-bert-2.0/config.json', 'w2v-bert-2.0/preprocessor_config.json',
            'w2v-bert-2.0/model.safetensors',
            'campplus_cn_common.bin', 'bigvgan/config.json', 'bigvgan/bigvgan_generator.pt')})
        # 显式传给 IndexTTS2，不能调用会自动下载／迁移的 ensure_models_available。
        paths = {'w2v_bert': str(aux / 'w2v-bert-2.0'),
                 'campplus': str(aux / 'campplus_cn_common.bin'), 'bigvgan': str(aux / 'bigvgan')}
        source = root / 'indextts' / 'infer_v2_5.py'
        variant = 'IndexTTS-2.5'
        packages = ('torch', 'torchaudio', 'transformers', 'soundfile', 'librosa', 'omegaconf', 'modelscope', 'safetensors')
    elif engine == 'gpt':
        base = root / 'GPT_SoVITS' / 'pretrained_models'
        files = {'s1v3.ckpt': base / 's1v3.ckpt', 'v2Pro/s2Gv2ProPlus.pth': base / 'v2Pro' / 's2Gv2ProPlus.pth'}
        files.update({name: aux / name for name in (
            'chinese-roberta-wwm-ext-large/config.json', 'chinese-roberta-wwm-ext-large/pytorch_model.bin',
            'chinese-roberta-wwm-ext-large/vocab.txt',
            'chinese-hubert-base/config.json', 'chinese-hubert-base/pytorch_model.bin',
            'sv/pretrained_eres2netv2w24s4ep4.ckpt')})
        paths = {'bert': str(aux / 'chinese-roberta-wwm-ext-large'), 'hubert': str(aux / 'chinese-hubert-base')}
        source = root / 'GPT_SoVITS' / 'TTS_infer_pack' / 'TTS.py'
        variant = 'v2ProPlus'
        packages = ('torch', 'torchaudio', 'transformers', 'soundfile', 'librosa', 'cn2an', 'sentencepiece', 'resampy', 'fast-langdetect', 'onnxruntime-gpu')
    else:
        raise ValueError('未知引擎。')
    files['engineSource'] = source
    if engine == 'gpt' and not files['chinese-roberta-wwm-ext-large/vocab.txt'].is_file():
        alternate = aux / 'chinese-roberta-wwm-ext-large' / 'tokenizer.json'
        if alternate.is_file():
            files['chinese-roberta-wwm-ext-large/vocab.txt'] = alternate
    if engine == 'gpt':
        files['chinese-hubert-base/preprocessor_config.json'] = aux / 'chinese-hubert-base/preprocessor_config.json'
    # Transformers 支持这两种本地权重格式；不因文件名不同误报缺失。
    for name, file in list(files.items()):
        if name.endswith('/pytorch_model.bin') and not file.is_file() and file.with_name('model.safetensors').is_file():
            files[name] = file.with_name('model.safetensors')
    if engine == 'index' and not files['w2v-bert-2.0/model.safetensors'].is_file():
        alternate = aux / 'w2v-bert-2.0' / 'pytorch_model.bin'
        if alternate.is_file():
            files['w2v-bert-2.0/model.safetensors'] = alternate
    missing, manifest, versions, errors = [], {}, {}, []
    for name, file in files.items():
        if not file.is_file() or not file.stat().st_size:
            missing.append(str(file))
        else:
            manifest[name] = {'path': str(file), 'size': file.stat().st_size}
            if file.suffix in ('.json', '.yaml', '.py'):
                manifest[name]['sha256'] = hashlib.sha256(file.read_bytes()).hexdigest()
            if file.suffix == '.json':
                try:
                    config = json.loads(file.read_text(encoding='utf-8'))
                    if not isinstance(config, dict):
                        errors.append(str(file) + '：配置必须为对象。')
                    expected = {'w2v-bert-2.0/config.json': 'wav2vec2-bert',
                                'chinese-roberta-wwm-ext-large/config.json': 'bert',
                                'chinese-hubert-base/config.json': 'hubert'}.get(name)
                    if expected and config.get('model_type') != expected:
                        errors.append(str(file) + '：model_type 与对应权重系列不匹配。')
                except (ValueError, AttributeError) as error:
                    errors.append(str(file) + '：' + str(error))
    if engine == 'index' and files['config.yaml'].is_file():
        try:
            import yaml
            config = yaml.safe_load(files['config.yaml'].read_text(encoding='utf-8'))
            if not isinstance(config, dict) or str(config.get('version')) != '2.5':
                errors.append('Index 配置必须明确为 version 2.5。')
        except Exception as error:
            errors.append('Index YAML 无法验证：' + str(error))
    for package in packages:
        try:
            versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            versions[package] = None
    generation_missing = []
    if engine == 'gpt':
        # v2ProPlus 的中文 cleaner 动态加载 chinese2，模型加载本身不会触发这些文件。
        generation_files = [aux / 'fast_langdetect/lid.176.bin', root / 'GPT_SoVITS/text/G2PWModel/POLYPHONIC_CHARS.txt',
                            root / 'GPT_SoVITS/text/G2PWModel/MONOPHONIC_CHARS.txt']
        g2pw = root / 'GPT_SoVITS/text/G2PWModel/g2pW.onnx'
        if not g2pw.is_file():
            g2pw = g2pw.with_name('g2pw.onnx')
        generation_files.append(g2pw)
        generation_missing = [str(file) for file in generation_files if not file.is_file() or not file.stat().st_size]
        for file in generation_files:
            if file.is_file() and file.stat().st_size:
                manifest['generation/' + file.name] = {'path': str(file), 'size': file.stat().st_size}
    return {'engine': engine, 'variant': variant, 'python': sys.version, 'packages': versions,
            'languageResources': inspect_gpt_language_resources() if engine == 'gpt' else {},
            'missingGenerationResources': generation_missing,
            'generationReady': not missing and not errors and all(versions.values()) and not generation_missing,
            'missingFiles': missing, 'manifest': manifest, 'auxiliaryPaths': paths,
            'configurationErrors': errors,
            'filesAndPackagesPresent': not missing and not errors and all(versions.values()),
            'weightsLoaded': False, 'runtimeVerified': False, 'networkUsed': False,
            'notes': '只核对必要文件与包元数据；文件存在不代表加载成功、音质或取消验收通过。'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', choices=('index', 'gpt'), required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--auxiliary')
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    try:
        result = inspect(args.engine, args.repository, args.auxiliary)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result['generationReady'] else 2
    except (ValueError, OSError) as error:
        print(json.dumps({'error': str(error), 'weightsLoaded': False}, ensure_ascii=False))
        return 2


if __name__ == '__main__':
    sys.exit(main())
