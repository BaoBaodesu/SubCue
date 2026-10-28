"""离线预检不加载模型，不把缺失资产伪装成可用引擎。"""
import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest
import zipfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('engine_preflight', Path(__file__).parents[2] / 'tools' / 'tts_engine_preflight.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EnginePreflightTests(unittest.TestCase):
    def test_optional_language_resources_report_all_missing_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = module.inspect_gpt_language_resources([root], root)
            self.assertEqual(len(report['English']['missing']), 5)
            self.assertEqual(len(report['Japanese']['missing']), 8)
            self.assertFalse(report['runtimeVerified'])
            self.assertFalse(report['qualityVerified'])
            self.assertEqual(list(root.iterdir()), [])

    def test_optional_resources_reject_broken_zip_and_find_existing_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'taggers').mkdir()
            legacy = root / 'taggers/averaged_perceptron_tagger.zip'
            legacy.write_bytes(b'broken zip')
            self.assertIn('taggers/averaged_perceptron_tagger.zip', module.inspect_gpt_language_resources([root], root)['English']['missing'])
            with zipfile.ZipFile(legacy, 'w') as archive:
                archive.writestr('averaged_perceptron_tagger/averaged_perceptron_tagger.pickle', b'fixture only')
            with zipfile.ZipFile(root / 'cmu.zip', 'w') as archive:
                archive.writestr('cmudict/cmudict', b'fixture only')
            (root / 'corpora').mkdir(); (root / 'cmu.zip').rename(root / 'corpora/cmudict.zip')
            tagger = root / 'taggers/averaged_perceptron_tagger_eng'; tagger.mkdir()
            for suffix in ('classes', 'tagdict', 'weights'):
                (tagger / ('averaged_perceptron_tagger_eng.' + suffix + '.json')).write_text('{}')
            report = module.inspect_gpt_language_resources([root], root)
            self.assertTrue(report['English']['filesPresent'])
            self.assertFalse(report['Japanese']['filesPresent'])
            self.assertFalse(report['runtimeVerified'])

    def test_missing_files_are_explicit_and_no_cache_created(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for engine in ('index', 'gpt'):
                report = module.inspect(engine, root)
                self.assertFalse(report['filesAndPackagesPresent'])
                self.assertFalse(report['weightsLoaded'])
                self.assertFalse(report['networkUsed'])
                self.assertTrue(report['missingFiles'])
                self.assertFalse(report['generationReady'])
                if engine == 'gpt':
                    self.assertEqual(len(report['missingGenerationResources']), 4)
            self.assertEqual(list(root.iterdir()), [])

    def test_existing_file_metadata_is_preserved_without_modification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'checkpoints').mkdir()
            config = root / 'checkpoints' / 'config.yaml'
            config.write_text('version: 2.5\n', encoding='utf-8')
            with patch.object(module.importlib.metadata, 'version', return_value='test'):
                report = module.inspect('index', root)
            self.assertIn('sha256', report['manifest']['config.yaml'])
            self.assertEqual(config.read_text(encoding='utf-8'), 'version: 2.5\n')
            self.assertFalse(report['runtimeVerified'])
            self.assertFalse(report['filesAndPackagesPresent'])

    def test_relative_paths_are_rejected(self):
        with self.assertRaises(ValueError):
            module.inspect('index', '.')
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                module.inspect('gpt', directory, 'relative')

    def test_wrong_index_version_is_not_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'checkpoints').mkdir()
            (root / 'checkpoints/config.yaml').write_text('version: 2\n', encoding='utf-8')
            report = module.inspect('index', root)
            self.assertTrue(report['configurationErrors'])
            self.assertFalse(report['filesAndPackagesPresent'])

    def test_gpt_tokenizer_json_and_wrong_model_type_are_distinguished(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bert = root / 'GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large'
            bert.mkdir(parents=True)
            (bert / 'tokenizer.json').write_text('{}', encoding='utf-8')
            (bert / 'config.json').write_text('{"model_type":"hubert"}', encoding='utf-8')
            report = module.inspect('gpt', root)
            self.assertEqual(report['manifest']['chinese-roberta-wwm-ext-large/vocab.txt']['path'], str(bert / 'tokenizer.json'))
            self.assertTrue(report['configurationErrors'])
            self.assertFalse(report['filesAndPackagesPresent'])

    def test_model_identity_uses_content_not_directory_location(self):
        sys.path.insert(0, str(Path(__file__).parents[2] / 'tools'))
        import tts_external_engines
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, second = root / 'first.pth', root / 'second.pth'
            first.write_bytes(b'fixture only; not a real checkpoint')
            second.write_bytes(first.read_bytes())
            def report(engine, path):
                return {'filesAndPackagesPresent': True, 'missingGenerationResources': [], 'packages': {'fixture': '1'},
                        'manifest': {'weights': {'path': str(path), 'size': path.stat().st_size}}}
            with patch.object(tts_external_engines, 'inspect', side_effect=report):
                initial = tts_external_engines.check('index-local', first)['modelVersion']
                self.assertEqual(initial, tts_external_engines.check('index-local', second)['modelVersion'])
                second.write_bytes(b'changed fixture')
                self.assertNotEqual(initial, tts_external_engines.check('index-local', second)['modelVersion'])


if __name__ == '__main__':
    unittest.main()
