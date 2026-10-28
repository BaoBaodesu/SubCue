import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid
import wave
import weakref
from types import SimpleNamespace
from unittest.mock import patch

WORKER = Path(__file__).resolve().parents[2] / 'tools' / 'tts_python_worker.py'


class TtsWorkerTests(unittest.TestCase):
    def test_gpt_explicitly_disables_acceleration_and_rejects_unexpected_backend(self):
        sys.path.insert(0, str(WORKER.parent))
        import tts_python_worker as worker
        import tts_external_engines as engines
        with patch.object(engines, 'inspect', return_value={'filesAndPackagesPresent': True,
                'missingGenerationResources': [], 'manifest': {}, 'packages': {}}), patch.dict(sys.modules, {
                'onnxruntime': SimpleNamespace(__version__='fixture', get_available_providers=lambda: ['CPUExecutionProvider'])}):
            checked = engines.check('gpt-local', 'fixture')
        self.assertEqual(checked['profile']['adapterVersion'], 3)
        self.assertFalse(checked['profile']['cudaGraph'])
        self.assertFalse(checked['profile']['flashAttention'])
        self.assertFalse(checked['profile']['acceleration'])
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.wav'; reference.write_bytes(b'fixture reference')
            job = self.job(referencePath=str(reference), referenceText='审核文本', referenceSha256=worker.digest(reference),
                           modelVersion=checked['modelVersion'], parameters={'speedFactor': 0.85})
            calls, events = [], []
            def run(parameters):
                calls.append(parameters)
                yield 24000, [0.1]
            # 上游若忽略关闭参数并持有加速实例，必须报错且不能发布候选文件。
            with patch.dict(sys.modules, {'torch': SimpleNamespace(cuda=SimpleNamespace(
                    reset_peak_memory_stats=lambda: None, empty_cache=lambda: None)),
                    'numpy': SimpleNamespace(), 'soundfile': SimpleNamespace()}), patch.dict(worker.os.environ), \
                    patch.object(engines, 'check', return_value=checked), \
                    patch.object(engines, 'load', return_value=SimpleNamespace(run=run, t2s_accel=object())), \
                    patch.object(worker, 'emit', side_effect=events.append), patch.object(engines.os, 'chdir'):
                engines.batch({'provider': 'gpt-local', 'modelDirectory': root, 'outputDirectory': root, 'jobs': [job]})
            self.assertEqual(len(calls), 1)
            self.assertFalse(calls[0]['use_cuda_graph'])
            self.assertFalse(calls[0]['use_flash_attention'])
            self.assertEqual(calls[0]['speed_factor'], 0.85)
            self.assertEqual([event['type'] for event in events], ['job_started', 'error', 'batch_finished'])
            self.assertIn('语义加速', events[1]['message'])
            self.assertEqual(list(Path(root).glob('*.wav')), [reference])

    def test_gpt_speed_is_bounded_and_other_engines_reject_it(self):
        sys.path.insert(0, str(WORKER.parent))
        import tts_python_worker as worker
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.wav'; reference.write_bytes(b'fixture reference')
            job = self.job(referencePath=str(reference), referenceText='审核文本', referenceSha256=worker.digest(reference))
            for speed in (0.5, 0.85, 1, 2):
                job['parameters'] = {'speedFactor': speed}
                worker.validate_jobs([job], False, gpt_controls=True)
                for options in ({}, {'index_controls': True}):
                    with self.assertRaises(ValueError): worker.validate_jobs([job], False, **options)
            for speed in (0.49, 2.01, True, '0.85', float('nan'), float('inf')):
                job['parameters'] = {'speedFactor': speed}
                with self.assertRaises(ValueError): worker.validate_jobs([job], False, gpt_controls=True)

    def test_qwen_failure_stops_batch_and_releases_resources(self):
        sys.path.insert(0, str(WORKER.parent))
        import tts_python_worker as worker

        # 执行真实 Qwen 适配分支，以轻量依赖注入验证异常和持有关系，不冒充模型音质测试。
        class Buffer:
            ndim = 1
            size = 16
            dtype = 'float32'
            def float(self): return self

        for phase in ('load', 'reference', 'generate', 'write', 'success'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as root:
                events, resources, calls, released = [], [], [], []
                def tracked(value):
                    resources.append(weakref.ref(value))
                    return value
                def empty_cache():
                    released.append(all(value() is None for value in resources))

                class Model:
                    def __init__(self):
                        self.model = SimpleNamespace(dtype='bf16', speech_tokenizer=SimpleNamespace(model=Buffer()))
                    @staticmethod
                    def from_pretrained(*args, **kwargs):
                        if phase == 'load': raise RuntimeError('fixture load failure')
                        return tracked(Model())
                    def create_voice_clone_prompt(self, **kwargs):
                        if phase == 'reference': raise ValueError('fixture reference failure')
                        return tracked(Buffer())
                    def generate_voice_clone(self, **kwargs):
                        calls.append({key: value for key, value in kwargs.items() if key != 'voice_clone_prompt'})
                        if phase == 'generate' and len(calls) == 2:
                            raise RuntimeError('fixture CUDA out of memory')
                        return [tracked(Buffer())], 24000

                def write(path, samples, rate, **kwargs):
                    if phase == 'write' and len(calls) == 2: raise OSError('fixture write failure')
                    Path(path).write_bytes(b'fixture audio')

                reference = Path(root) / 'reference.wav'
                reference.write_bytes(b'fixture reference')
                jobs = [self.job(spokenText=str(index), language=('Chinese', 'English', 'Japanese')[index] if phase == 'success' else 'Chinese', referencePath=str(reference), referenceText='审核文本',
                    referenceSha256=worker.digest(reference), modelVersion='fixture') for index in range(3)]
                with patch.dict(sys.modules, {
                    'torch': SimpleNamespace(bfloat16='bf16', float16='fp16', cuda=SimpleNamespace(
                        is_available=lambda: True, is_bf16_supported=lambda: True, reset_peak_memory_stats=lambda: None,
                        empty_cache=empty_cache, get_device_name=lambda index: 'fixture',
                        max_memory_allocated=lambda: 0, max_memory_reserved=lambda: 0)),
                    'numpy': SimpleNamespace(isfinite=lambda value: SimpleNamespace(all=lambda: True)),
                    'soundfile': SimpleNamespace(read=lambda *args, **kwargs: (tracked(Buffer()), 24000), write=write),
                    'qwen_tts': SimpleNamespace(Qwen3TTSModel=Model)}), patch.dict(worker.os.environ), \
                    patch.object(worker, 'emit', side_effect=events.append), patch.object(worker, 'preflight', return_value={
                        'modelVersion': 'fixture', 'packages': {}, 'inferenceProfile': {'nonStreamingMode': False,
                            'chineseTalkerDoSample': False, 'chineseRepetitionPenalty': 1.15}}):
                    request = {'provider': 'qwen-local', 'modelDirectory': root, 'outputDirectory': root, 'jobs': jobs}
                    if phase == 'load':
                        with self.assertRaisesRegex(RuntimeError, 'fixture load failure'): worker.batch(request)
                    else:
                        worker.batch(request)
                self.assertEqual(released, [True], '清理 CUDA 缓存前仍持有模型、参考条件或波形')
                errors = [event for event in events if event['type'] == 'error']
                results = [event for event in events if event['type'] == 'result']
                if phase == 'success':
                    self.assertEqual(len(results), 3)
                    self.assertEqual(errors, [])
                    self.assertIs(calls[0]['do_sample'], False)
                    self.assertEqual(calls[0]['repetition_penalty'], 1.15)
                    for call in calls[1:]:
                        self.assertNotIn('do_sample', call)
                        self.assertNotIn('repetition_penalty', call)
                elif phase == 'load':
                    self.assertEqual(events, [])
                else:
                    self.assertEqual(len(errors), 1)
                    self.assertIn('Traceback', errors[0]['traceback'])
                    self.assertEqual(errors[0]['code'], 'oom' if phase == 'generate' else 'generation_failed')
                    self.assertEqual(len(results), 0 if phase == 'reference' else 1)
                    self.assertFalse(any(event.get('jobId') == jobs[2]['jobId'] for event in events))
                self.assertEqual(len(list(Path(root).glob('.tts-*.part'))), 0)
                self.assertEqual((Path(root) / (jobs[0]['jobId'] + '.wav')).exists(), len(results) > 0)
                for event in results:
                    self.assertEqual(Path(event['audioPath']).read_bytes(), b'fixture audio')

    def test_target_and_reference_languages_are_explicit_and_engine_limited(self):
        sys.path.insert(0, str(WORKER.parent))
        import tts_python_worker as worker
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.wav'; reference.write_bytes(b'reference')
            for language in ('Chinese', 'English', 'Japanese'):
                job = self.job(language=language, referenceLanguage='Chinese', referenceText='人工确认参考', referencePath=str(reference), referenceSha256=worker.digest(reference))
                worker.validate_jobs([job], False, languages=('Chinese', 'English', 'Japanese'))
                if language != 'Chinese':
                    with self.assertRaises(ValueError): worker.validate_jobs([job], False)
            job['referenceLanguage'] = 'Auto'
            with self.assertRaises(ValueError): worker.validate_jobs([job], False, languages=('Chinese', 'English', 'Japanese'))
    def test_index_controls_are_validated_before_loading_and_other_engines_reject(self):
        sys.path.insert(0, str(WORKER.parent))
        import tts_python_worker as worker
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.wav'
            reference.write_bytes(b'reference')
            job = self.job(referencePath=str(reference), referenceSha256=worker.digest(reference), parameters={'durationFactor': 1.15, 'emotion': 'happy', 'emotionStrength': 0.2})
            worker.validate_jobs([job], False, transcript_required=False, index_controls=True)
            with self.assertRaises(ValueError): worker.validate_jobs([job], True)
            annotated = self.job(spokenText='小<变化|BIAN4 HUA4>。')
            with self.assertRaises(ValueError): worker.validate_jobs([annotated], True)
            for invalid in ({'durationFactor': float('nan')}, {'durationFactor': True}, {'durationFactor': 2.01}, {'emotion': 'happy'}, {'emotion': 'unknown', 'emotionStrength': .2}, {'emotion': 'happy', 'emotionStrength': 0}):
                job['parameters'] = invalid
                with self.assertRaises(ValueError): worker.validate_jobs([job], False, transcript_required=False, index_controls=True)
    def job(self, **updates):
        result = {'jobId': str(uuid.uuid4()), 'spokenText': 'Mock 测试，不是真实配音',
                  'language': 'Chinese', 'inputHash': 'ab' * 32}
        result.update(updates)
        return result

    def run_batch(self, root, jobs):
        result = subprocess.run([sys.executable, '-B', str(WORKER)],
            input=json.dumps({'action': 'batch', 'provider': 'mock', 'outputDirectory': str(root), 'jobs': jobs}),
            text=True, encoding='utf-8', capture_output=True, timeout=10)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        return result, [event for event in events if event['type'] != 'job_started']

    def test_valid_mock_audio_and_atomic_no_overwrite(self):
        with tempfile.TemporaryDirectory() as root:
            job = self.job()
            process, events = self.run_batch(root, [job])
            self.assertEqual(process.returncode, 0)
            self.assertTrue(events[0]['mock'])
            self.assertEqual(events[0]['inputHash'], job['inputHash'])
            path = Path(events[0]['audioPath'])
            with wave.open(str(path), 'rb') as audio:
                self.assertEqual((audio.getframerate(), audio.getnchannels(), audio.getnframes()), (24000, 1, 12000))
            original = path.read_bytes()
            _, events = self.run_batch(root, [job])
            self.assertEqual(events[0]['type'], 'error')
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(len(list(Path(root).iterdir())), 1)

    def test_complete_batch_validated_before_writing(self):
        with tempfile.TemporaryDirectory() as root:
            job = self.job()
            process, events = self.run_batch(root, [job, job])
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(events[0]['type'], 'fatal')
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_unsupported_instructions_are_not_silently_applied(self):
        with tempfile.TemporaryDirectory() as root:
            process, events = self.run_batch(root, [self.job(parameters={'instruct': '自由表演'})])
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(events[0]['type'], 'fatal')

    def test_oom_is_an_error_and_next_job_can_complete(self):
        with tempfile.TemporaryDirectory() as root:
            _, events = self.run_batch(root, [self.job(simulate='oom'), self.job()])
            self.assertEqual(events[0]['code'], 'oom')
            self.assertEqual(events[1]['type'], 'result')
            self.assertTrue(events[1]['mock'])

    def test_worker_crash_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as root:
            process, events = self.run_batch(root, [self.job(simulate='crash')])
            self.assertEqual(process.returncode, 18)
            self.assertEqual(events, [])  # 开始事件不代表成功；崩溃没有结果或结束事件。

    def test_cancel_waits_for_child_and_leaves_no_audio(self):
        with tempfile.TemporaryDirectory() as root:
            request = {'action': 'batch', 'provider': 'mock', 'outputDirectory': root,
                       'jobs': [self.job(simulate='delay')]}
            with subprocess.Popen([sys.executable, '-B', str(WORKER)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                try:
                    process.communicate(json.dumps(request).encode('utf-8'), timeout=0.5)
                    self.fail('模拟延迟任务不应提前完成')
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)
                self.assertIsNotNone(process.returncode)
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_missing_local_model_fails_without_fallback(self):
        result = subprocess.run([sys.executable, '-B', str(WORKER)], input=json.dumps({
            'action': 'check', 'modelDirectory': 'not-a-local-model'}), text=True,
            encoding='utf-8', capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['type'], 'fatal')

    def test_preflight_identity_and_offline_guard(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            files = ('config.json', 'generation_config.json', 'tokenizer_config.json', 'vocab.json', 'merges.txt',
                     'preprocessor_config.json', 'model.safetensors', 'speech_tokenizer/config.json',
                     'speech_tokenizer/preprocessor_config.json', 'speech_tokenizer/model.safetensors')
            for name in files:
                path = directory / name
                path.parent.mkdir(exist_ok=True)
                path.write_text('{}', encoding='utf-8')
            (directory / 'config.json').write_text(json.dumps({'tts_model_type': 'base',
                'tokenizer_type': 'qwen3_tts_tokenizer_12hz', 'tts_model_size': '0b6',
                'talker_config': {'codec_language_id': {'chinese': 2055, 'english': 2050, 'japanese': 2058}}}), encoding='utf-8')
            code = '''
import runpy, sys, socket
from pathlib import Path
worker = runpy.run_path(sys.argv[1])
worker['importlib'].metadata.version = lambda name: 'test-version'
worker['importlib'].metadata.requires = lambda name: []
root = Path(sys.argv[2])
first = worker['preflight'](root)
assert not first['weightsLoaded'] and first['offline']
worker['importlib'].metadata.requires = lambda name: ['huggingface-hub>=0.34.0,<1.0'] if name == 'transformers' else []
worker['importlib'].metadata.version = lambda name: '2.2.0' if name == 'huggingface-hub' else 'test-version'
try:
    worker['preflight'](root)
except ValueError as error:
    assert 'huggingface-hub' in str(error)
else:
    raise AssertionError('incompatible environment was accepted')
worker['importlib'].metadata.requires = lambda name: []
worker['importlib'].metadata.version = lambda name: 'test-version'
(root / 'model.safetensors').write_text('changed fixture')
assert first['modelVersion'] != worker['preflight'](root)['modelVersion']
(root / 'speech_tokenizer/model.safetensors').unlink()
try:
    worker['preflight'](root)
except ValueError:
    pass
else:
    raise AssertionError('missing tokenizer was accepted')
try:
    socket.create_connection(('127.0.0.1', 1))
except RuntimeError:
    pass
else:
    raise AssertionError('offline guard failed')
'''
            result = subprocess.run([sys.executable, '-B', '-c', code, str(WORKER), str(directory)],
                                    text=True, encoding='utf-8', capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
