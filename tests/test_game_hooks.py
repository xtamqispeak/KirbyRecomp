from pathlib import Path
import hashlib
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import game_hooks

class HookTests(unittest.TestCase):
    def fixture(self,folder):
        source=folder/'main/main_000.c';source.parent.mkdir()
        # Original synthetic C markers only; no executable game instructions.
        source.write_text('\n'.join(f'void fake_{i}(Cpu *c) {{\n    FN_ENTER(0x{site:x}ull);\n}}' for i,site in enumerate(game_hooks.SITES)))
        manifest={'modules':[{'name':'main','base':'0x7000010000','build_id':game_hooks.BUILD.ljust(64,'0')}],
                  'generated_sources':{'main/main_000.c':hashlib.sha256(source.read_bytes()).hexdigest()}}
        return source,manifest
    def test_version_guard_and_idempotence(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory);source,manifest=self.fixture(folder)
            original=source.read_bytes();manifest['modules'][0]['build_id']='00'*32
            self.assertFalse(game_hooks.apply(folder,manifest));self.assertEqual(source.read_bytes(),original)
            manifest['modules'][0]['build_id']=game_hooks.BUILD.ljust(64,'0')
            self.assertTrue(game_hooks.apply(folder,manifest));patched=source.read_bytes()
            self.assertEqual(patched.count(b'extern int kirby_frame_hook'),len(game_hooks.SITES))
            self.assertTrue(game_hooks.apply(folder,manifest));self.assertEqual(source.read_bytes(),patched)
    def test_modified_source_and_incomplete_layout_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory);source,manifest=self.fixture(folder)
            source.write_text('modified')
            with self.assertRaisesRegex(ValueError,'source changed'):game_hooks.apply(folder,manifest)
            manifest['generated_sources']['main/main_000.c']=hashlib.sha256(source.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError,'function entry'):game_hooks.apply(folder,manifest)
            self.assertEqual(source.read_text(),'modified')
