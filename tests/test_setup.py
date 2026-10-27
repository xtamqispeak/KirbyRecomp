from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
from fixtures import write_game
from setup_kirby import validate


class SetupTests(unittest.TestCase):
    def test_separate_exefs_and_romfs(self):
        with tempfile.TemporaryDirectory() as directory:
            game = write_game(Path(directory) / 'dump', title_id=0x01003FB00C5A8000)
            for name in ('gfx', 'map', 'mint', 'msg', 'snd'):
                (game / 'romfs' / name).mkdir()
            report, exefs = validate(game / 'exefs', game / 'romfs')
            self.assertEqual(exefs, game / 'exefs')
            self.assertTrue(report['title_id_verified'])
            (game / 'romfs' / 'snd').rmdir()
            with self.assertRaisesRegex(ValueError, 'snd/'):
                validate(game / 'exefs', game / 'romfs')

    def test_other_game_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            game = write_game(Path(directory) / 'dump')
            with self.assertRaises(ValueError):
                validate(game / 'exefs', game / 'romfs')
