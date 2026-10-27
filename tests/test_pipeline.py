import argparse
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))
import switchrecomp as mk8d
from fixtures import write_game, write_nso, write_npdm, write_pfs0


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.Nso, _ = mk8d.dependencies()

    def test_compressed_and_uncompressed_images_match(self):
        a = self.Nso(write_nso(self.path / 'a'))
        b = self.Nso(write_nso(self.path / 'b', compressed=True))
        self.assertEqual(a.image, b.image)
        self.assertEqual(len(a.build_id), 64)
        self.assertTrue(a.build_id.endswith('00000000'))

    def test_corrupt_segment_hash_rejected(self):
        path = write_nso(self.path / 'main')
        data = bytearray(path.read_bytes())
        data[0x110] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            self.Nso(path)

    def test_out_of_file_segment_rejected(self):
        path = write_nso(self.path / 'main')
        data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 0x10, len(data))
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'outside NSO'):
            self.Nso(path)

    def test_oversized_allocation_rejected(self):
        path = write_nso(self.path / 'main')
        data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 0x3C, 0xFFFFFFFF)
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, '512 MiB'):
            self.Nso(path)

    def test_multimodule_layout_and_imports(self):
        game = write_game(self.path / 'game', extra_modules=True)
        report = mk8d.analyze_game(game)
        self.assertEqual([x['name'] for x in report['modules']], ['main', 'subsdk0', 'sdk'])
        for a, b in zip(report['modules'], report['modules'][1:]):
            self.assertLessEqual(int(a['base'], 16) + a['image_size'], int(b['base'], 16))
        self.assertEqual(report['unresolved_strong_imports'], ['missing_test_symbol'])
        self.assertEqual(report['modules'][0]['imports'][1]['resolution'], 'weak_zero')
        self.assertTrue(report['title_id_verified'])
        self.assertFalse(report['playable'])

    def test_foreign_title_rejected(self):
        game = write_game(self.path / 'game')
        write_npdm(game / 'exefs' / 'main.npdm', 0x1234)
        with self.assertRaisesRegex(ValueError, 'Wrong title'):
            mk8d.analyze_game(game)

    def test_kirby_profile_checks_title_id(self):
        game = write_game(self.path / 'game', title_id=0x01003FB00C5A8000)
        report = mk8d.analyze_game(game, profile='kirby')
        self.assertEqual(report['architecture'], 'ARM64')
        self.assertEqual(report['game'], 'Super Kirby Clash')
        with self.assertRaisesRegex(ValueError, 'Wrong title'):
            mk8d.analyze_game(game, profile='mk8d')

    def test_arm32_metadata_and_elf32_symbols(self):
        game = write_game(self.path / 'game', bits=32)
        report = mk8d.analyze_game(game)
        self.assertEqual(report['architecture'], 'ARM32')
        self.assertEqual(report['unresolved_strong_imports'], ['missing_test_symbol'])
        args = argparse.Namespace(game=str(game), out=str(self.path / 'output'), reference=None, chunk=1500)
        with self.assertRaisesRegex(ValueError, 'ARM32'):
            mk8d.generate_game(args)

    def test_missing_npdm_can_be_analyzed_but_not_generated(self):
        game = write_game(self.path / 'game')
        (game / 'exefs' / 'main.npdm').unlink()
        self.assertFalse(mk8d.analyze_game(game)['title_id_verified'])
        args = argparse.Namespace(game=str(game), out=str(self.path / 'output'), reference=None, chunk=1500)
        with self.assertRaisesRegex(ValueError, 'main.npdm'):
            mk8d.generate_game(args)

    def test_existing_output_and_input_output_rejected(self):
        game = write_game(self.path / 'game')
        for out in (game / 'exefs' / 'generated', game):
            args = argparse.Namespace(game=str(game), out=str(out), reference=None, chunk=1500)
            with self.assertRaisesRegex(ValueError, 'Output must be separate'):
                mk8d.generate_game(args)
        output = self.path / 'existing'
        output.mkdir()
        (output / 'keep.txt').write_text('keep')
        args.out = str(output)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            mk8d.generate_game(args)
        self.assertEqual((output / 'keep.txt').read_text(), 'keep')

    def test_unknown_nso_module_is_not_silently_skipped(self):
        game = write_game(self.path / 'game')
        write_nso(game / 'exefs' / 'other_module')
        with self.assertRaisesRegex(ValueError, 'Unexpected NSO'):
            mk8d.analyze_game(game)

    def test_pfs_inventory(self):
        path = self.path / 'sample.nsp'
        write_pfs0(path, [('test.nca', b'test payload'), ('test.tik', b'untouched')])
        report = mk8d.probe_package(path)
        self.assertEqual(report['nca_count'], 1)
        self.assertFalse(report['ready_for_recompilation'])

    def test_truncated_pfs_rejected(self):
        path = self.path / 'sample.nsp'
        write_pfs0(path, [('test.nca', b'1234')])
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, 'outside package'):
            mk8d.probe_package(path)

    def test_unsafe_pfs_name_rejected(self):
        path = self.path / 'sample.nsp'
        write_pfs0(path, [('../file', b'1234')])
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            mk8d.probe_package(path)


if __name__ == '__main__':
    unittest.main()
