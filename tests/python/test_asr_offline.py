import importlib.util
from pathlib import Path
import socket
import unittest


class AsrOfflineTests(unittest.TestCase):
    def test_offline_request_blocks_implicit_network_before_model_import(self):
        path = Path(__file__).resolve().parents[2] / 'tools' / 'asr_python_worker.py'
        spec = importlib.util.spec_from_file_location('asr_worker', path)
        worker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(worker)
        original = (socket.create_connection, socket.socket.connect, socket.socket.connect_ex)
        try:
            self.assertIn('python', worker.run_request({'action': 'ping', 'offlineOnly': True}))
            with self.assertRaisesRegex(RuntimeError, 'Offline sample transcription'):
                socket.create_connection(('example.invalid', 443))
            with socket.socket() as client:
                with self.assertRaisesRegex(RuntimeError, 'Offline sample transcription'):
                    client.connect(('127.0.0.1', 9))
        finally:
            socket.create_connection, socket.socket.connect, socket.socket.connect_ex = original


if __name__ == '__main__':
    unittest.main()
