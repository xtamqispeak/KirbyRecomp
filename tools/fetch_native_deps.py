"""Fetch native graphics libraries into this project, without installing MSYS2.

Uses the official MSYS2 UCRT64 repository and verifies package SHA-256 values
against its repository database. Packages stay in build/native-deps.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor

import zstandard

ROOT = Path(__file__).resolve().parents[1]
BASE = 'https://repo.msys2.org/mingw/ucrt64/'
PREFIX = 'mingw-w64-ucrt-x86_64-'
TARGETS = ['sdl3', 'vulkan-headers', 'vulkan-loader', 'vulkan-memory-allocator', 'shaderc', 'winpthreads']


def unpack(raw):
    if raw[:4] == b'\x28\xb5\x2f\xfd':
        raw = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(raw)).read()
    return tarfile.open(fileobj=io.BytesIO(raw), mode='r:*')


def main():
    destination = ROOT / 'build' / 'native-deps'
    destination.mkdir(parents=True, exist_ok=True)
    packages = {}
    with unpack(urllib.request.urlopen(BASE + 'ucrt64.db', timeout=60).read()) as database:
        for member in database:
            if not member.name.endswith('/desc'):
                continue
            desc = database.extractfile(member).read().decode()
            fields = {}
            for block in desc.strip().split('\n\n'):
                lines = block.splitlines()
                if lines and lines[0].startswith('%'):
                    fields[lines[0].strip('%')] = lines[1:]
            packages[fields['NAME'][0]] = fields
    providers = {}
    for package_name, package in packages.items():
        for provided in package.get('PROVIDES', []):
            providers.setdefault(re.split(r'[<>=]', provided)[0], []).append(package_name)
    selected = {}
    def require(name):
        name = re.split(r'[<>=]', name)[0]
        if name in selected:
            return
        if name not in packages:
            matches = providers.get(name, [])
            if len(matches) != 1:
                raise RuntimeError(f'Missing or ambiguous repository dependency: {name}: {matches}')
            name = matches[0]
            if name in selected:
                return
        package = packages[name]
        selected[name] = package
        for dependency in package.get('DEPENDS', []):
            require(dependency)
    for target in TARGETS:
        require(PREFIX + target)
    total = sum(int(p['CSIZE'][0]) for p in selected.values())
    print(f'{len(selected)} portable packages, {total / 1048576:.1f} MiB download', flush=True)
    lock = []
    def fetch(item):
        name, package = item
        filename, digest = package['FILENAME'][0], package['SHA256SUM'][0]
        cached = destination / filename
        if not cached.exists() or hashlib.sha256(cached.read_bytes()).hexdigest() != digest:
            data = urllib.request.urlopen(BASE + filename, timeout=120).read()
            if hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError(f'Hash mismatch: {filename}')
            cached.write_bytes(data)
        print(f'Fetched {name}', flush=True)
        return name, package, cached
    with ThreadPoolExecutor(max_workers=4) as workers:
        downloaded = list(workers.map(fetch, selected.items()))
    # Extract sequentially: shared parent directories must not race.
    for name, package, cached in downloaded:
        with unpack(cached.read_bytes()) as archive:
            members = [m for m in archive if m.name.startswith('ucrt64/') and (m.isfile() or m.isdir())]
            archive.extractall(destination, members=members, filter='data')
        lock.append({'name': name, 'version': package['VERSION'][0], 'filename': cached.name,
                     'sha256': package['SHA256SUM'][0], 'source': BASE + cached.name})
    (destination / 'packages.json').write_text(json.dumps(lock, indent=2) + '\n')
    print('Libraries:', destination / 'ucrt64', flush=True)


if __name__ == '__main__':
    main()
