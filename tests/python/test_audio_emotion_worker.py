"""审核策略的合成 fixture 测试，不证明真实情绪准确率。"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from audio_emotion_worker import prediction


class EmotionPolicyTests(unittest.TestCase):
    def test_low_score_requires_review(self):
        self.assertTrue(prediction([0.3, 0, 0, 0.3, 0.4, 0, 0, 0, 0])['requiresReview'])

    def test_small_margin_requires_review(self):
        self.assertTrue(prediction([0.5, 0, 0, 0, 0.5, 0, 0, 0, 0])['requiresReview'])

    def test_unknown_and_other_require_review(self):
        for index in (5, 8):
            scores = [0.] * 9
            scores[index] = 1.
            self.assertTrue(prediction(scores)['requiresReview'])

    def test_high_score_is_only_suggestion(self):
        result = prediction([0, 0, 0, 0, 1., 0, 0, 0, 0])
        self.assertFalse(result['requiresReview'])
        self.assertFalse(result['reviewed'])
        self.assertEqual(result['label'], 'neutral')
        self.assertNotIn('manualEmotion', result)

    def test_invalid_distribution_rejected(self):
        for scores in ([], [1.] * 9, [float('nan')] * 9, [-1.] * 9):
            with self.assertRaises(ValueError):
                prediction(scores)


if __name__ == '__main__':
    unittest.main()
