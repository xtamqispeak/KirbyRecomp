"""Exercise the actual precompiled setup and audit its extracted payload."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import switchrecomp as sr


def main():
    exe = ROOT / 'release/KirbyRecompSetup-v0.2.0-alpha.exe'
    data = exe.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe+4] == b'PE\0\0'
    assert struct.unpack_from('<H', data, pe+24+68)[0] == 2, 'Setup must be a GUI executable'
    with tempfile.TemporaryDirectory(prefix='setup-check-', dir=ROOT/'build') as temporary:
        base = Path(temporary).resolve()
        assert base.is_relative_to((ROOT / 'build').resolve())
        install = base / 'folder with spaces & punctuation'
        result = subprocess.run([str(exe), '--self-test', str(install)], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'Setup payload OK:' in result.stdout, result.stdout
        manifest = json.loads((install/'SETUP-PAYLOAD-MANIFEST.json').read_text())
        for name, digest in manifest.items():
            path = (install / name).resolve()
            assert path.is_relative_to(install)
            assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
            if not name.startswith('python/'):
                assert path.suffix not in ('.exe', '.dll', '.image', '.nsp', '.nca', '.keys', '.o')
        assert (install/'python/LICENSE.txt').is_file()
        assert (install/'LICENSE').is_file()
        assert (install/'vendor/lasrecomp/LICENSE').is_file()
        blocked = base / 'existing unrelated folder'
        blocked.mkdir()
        sentinel = blocked / 'sentinel.txt'
        sentinel.write_text('preserve')
        reject = subprocess.run([str(exe), '--self-test', str(blocked)], capture_output=True,
                                text=True, encoding='utf-8', timeout=10)
        assert reject.returncode != 0
        assert sentinel.read_text() == 'preserve'
        assert not (blocked/'kirby-setup.marker').exists()
    print('PASS GUI EXE, standalone Python, payload hashes/licenses, paths with spaces, and nonempty-folder guard')
    sr.save_json(ROOT/'reports/installer-tests.json', {'result': 'passed', 'game_files_included': False})


if __name__ == '__main__':
    main()
