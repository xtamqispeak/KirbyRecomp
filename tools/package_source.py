"""Package tracked source only; never include locally generated game material."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOP_FILES = {'.gitignore', '.gitattributes', 'README.md', 'LICENSE', 'requirements.txt',
             'requirements-build.txt', 'switchrecomp.py', 'Build Kirby.cmd', 'Play Kirby.cmd'}
SOURCE_DIRS = {'runtime', 'tools', 'tests', 'vendor', 'docs', '.github'}
EXTENSIONS = {'.c', '.cpp', '.h', '.py', '.md', '.txt', '.hlsl', '.yml', '.yaml'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='v0.1.0-alpha')
    args = parser.parse_args()
    if not re.fullmatch(r'v[0-9A-Za-z.-]+', args.version):
        parser.error('Invalid version')
    subprocess.run(['git', 'diff', '--exit-code', 'HEAD'], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    entries = []
    for name in sorted(filter(None, files)):
        relative = PurePosixPath(name)
        path = ROOT / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Not a regular source file: {name}')
        if name not in TOP_FILES:
            if relative.parts[0] not in SOURCE_DIRS or (
                path.suffix not in EXTENSIONS and path.name != 'LICENSE'
            ):
                raise ValueError(f'Unapproved release file: {name}')
        if any(p.lower() in {'exefs', 'romfs', '__pycache__', 'save'} or
               p.lower().startswith('generated') for p in relative.parts):
            raise ValueError(f'Private build material: {name}')
        data = subprocess.check_output(['git', 'show', f'HEAD:{name}'], cwd=ROOT)
        if path.suffix == '.cmd':
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        if b'\0' in data or data.startswith((b'MZ', b'NSO0', b'NCA3', b'PFS0', b'\x7fELF')):
            raise ValueError(f'Binary content in source release: {name}')
        if re.search(rb'(?:github_pat_|ghp_)[A-Za-z0-9_]{20,}', data):
            raise ValueError(f'Credential-like content: {name}')
        if re.search(rb'[A-Za-z]:[\\/]Users[\\/]', data, re.IGNORECASE):
            raise ValueError(f'User-specific absolute path: {name}')
        entries.append((name, data))
    destination = ROOT / 'release'
    destination.mkdir(exist_ok=True)
    prefix = f'KirbyRecomp-{args.version}'
    archive = destination / f'{prefix}-source-kit.zip'
    checksums = []
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, data in entries:
            bundle.writestr(f'{prefix}/{name}', data)
            checksums.append(f'{hashlib.sha256(data).hexdigest()}  {name}')
        bundle.writestr(f'{prefix}/SOURCE-MANIFEST.sha256', '\n'.join(checksums) + '\n')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'{archive}: {len(entries)} source files, {archive.stat().st_size} bytes')
    print(f'SHA-256: {digest}')


if __name__ == '__main__':
    main()
