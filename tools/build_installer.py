"""Build the Windows setup EXE, bundling source and a pinned portable Python."""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import switchrecomp as sr
from package_source import TOP_FILES, SOURCE_DIRS, EXTENSIONS

VERSION = 'v0.2.0-alpha'
PYTHON = ('python-3.13.17-embed-amd64.zip',
          'https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip',
          '9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38')
PIP = ('pip-25.3-py3-none-any.whl',
       'https://files.pythonhosted.org/packages/44/3c/d717024885424591d5376220b5e836c2d5293ce2011523c9de23ff7bf068/pip-25.3-py3-none-any.whl',
       '9655943313a94722b7774661c21049070f6bbb0a1516bf02f7c8d5d9201514cd')


def fetch(item):
    name, url, digest = item
    path = ROOT / 'build/installer-cache' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or sr.sha256(path) != digest:
        data = urllib.request.urlopen(url, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'Download hash mismatch: {name}')
        path.write_bytes(data)
    return path


def archive_files(path):
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename
            if name.startswith(('/', '\\')) or any(p in ('', '..', '.') for p in name.split('/')) or '\\' in name:
                raise ValueError(f'Unsafe dependency archive entry: {name}')
            yield name, archive.read(info)


def main():
    build = ROOT / 'build/installer'
    build.mkdir(parents=True, exist_ok=True)
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    entries = {}
    for name in filter(None, files):
        path = ROOT / name
        if name not in TOP_FILES and (Path(name).parts[0] not in SOURCE_DIRS or
                                      path.suffix not in EXTENSIONS and path.name != 'LICENSE'):
            raise ValueError(f'Unexpected tracked source: {name}')
        data = path.read_bytes()
        if path.is_symlink() or b'\0' in data or data.startswith((b'MZ', b'NSO0', b'NCA3', b'PFS0', b'\x7fELF')):
            raise ValueError(f'Non-source payload: {name}')
        if re.search(rb'(?:github_pat_|ghp_)[A-Za-z0-9_]{20,}', data):
            raise ValueError(f'Credential-like content in source: {name}')
        if re.search(rb'[A-Za-z]:[\\/]Users[\\/]', data, re.IGNORECASE):
            raise ValueError(f'User-specific path in source: {name}')
        entries[name] = data
    source_count = len(entries)
    for name, data in archive_files(fetch(PYTHON)):
        if name.endswith('._pth'):
            data = (b'python313.zip\n.\nLib/site-packages\n..\n'
                    b'..\\tools\n..\\vendor\\lasrecomp\\tools\nimport site\n')
        entries['python/' + name] = data
    for name, data in archive_files(fetch(PIP)):
        entries['python/Lib/site-packages/' + name] = data
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(entries.items())}
    entries['SETUP-PAYLOAD-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    payload = build / 'payload.zip'
    with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            archive.writestr(name, data)
    rc = build / 'payload.rc'
    rc.write_text(f'101 RCDATA "{payload.as_posix()}"\n1 24 "{(ROOT / "installer/app.manifest").as_posix()}"\n', encoding='utf-8')
    resource = build / 'payload.res'
    subprocess.run([sys.executable, '-m', 'ziglang', 'rc', '/fo', str(resource), str(rc)], check=True)
    release = ROOT / 'release'
    release.mkdir(exist_ok=True)
    exe = release / f'KirbyRecompSetup-{VERSION}.exe'
    command = sr.compiler_command() + ['-std=c11', '-O2', '-s', '-municode', '-Wl,--subsystem,windows',
        str(ROOT / 'installer/setup.c'), str(resource),
        '-luser32', '-lgdi32', '-lshell32', '-lole32', '-lcomdlg32', '-o', str(exe)]
    subprocess.run(command, check=True)
    digest = sr.sha256(exe)
    exe.with_suffix('.exe.sha256').write_text(f'{digest}  {exe.name}\n')
    sr.save_json(ROOT / 'reports/installer-build.json', {'version': VERSION,
        'source_files': source_count, 'payload_files': len(entries), 'exe': str(exe),
        'sha256': digest, 'python': PYTHON[0], 'game_files_included': False})
    print(f'{exe}: {exe.stat().st_size:,} bytes\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
