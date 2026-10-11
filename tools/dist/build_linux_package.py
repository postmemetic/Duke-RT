#!/usr/bin/env python3
"""Stage a Linux player package from a compiled, dependency-complete runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build(runtime, product, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='duke-package-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'duke-rt-linux-x86_64'
        stage.mkdir()
        required = ['raze', 'raze.pk3', 'libNRI.so', 'libzmusiclite.so',
                    'libzmusiclite.so.1', 'libzmusiclite.so.1.3.0', 'gamecontrollerdb.txt']
        for pattern in ('libnvidia-ngx-dlss.so.*', 'libnvidia-ngx-dlssd.so.*'):
            matches = sorted(runtime.glob(pattern))
            if len(matches) != 1:
                raise ValueError(f'Expected one {pattern} runtime library')
            required.append(matches[0].name)
        for name in required:
            shutil.copyfile(runtime / name, stage / name)
        for name in ('soundfonts', 'licenses'):
            shutil.copytree(runtime / name, stage / name)
        manifest_path = runtime / 'shaders/nri/nri-shaders.json'
        manifest = json.loads(manifest_path.read_text())
        if manifest['resolvedProfile'] != 'PRODUCTION' or len(manifest['entries']) != manifest['canonicalBlobCount']:
            raise ValueError('Production shader manifest is incomplete')
        shaders = stage / 'shaders/nri'
        shaders.mkdir(parents=True)
        for entry in manifest['entries']:
            name = entry['path']
            if Path(name).name != name or sha(runtime / 'shaders/nri' / name) != entry['sha256']:
                raise ValueError(f'Shader checksum/path mismatch: {name}')
            shutil.copyfile(runtime / 'shaders/nri' / name, shaders / name)
        shutil.copyfile(manifest_path, shaders / manifest_path.name)
        shutil.copytree(product / 'package/linux', stage, dirs_exist_ok=True)
        shutil.copyfile(product / 'LINUX.md', stage / 'LINUX.md')
        helper = stage / 'tools/dist/prepare_linux_content.py'
        helper.parent.mkdir(parents=True)
        shutil.copyfile(product / 'tools/dist/prepare_linux_content.py', helper)
        # Only the authored baseline belongs in a public package. Optional imports
        # always live in userdata and are never taken from the build machine.
        overlay = product / 'release-overlay'
        for name in ('LIGHTOVR', 'VOXELPRELOAD'):
            destination = stage / 'release-overlay' / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(overlay / name, destination)
        for channel in ('glowmaps', 'metallic', 'roughness', 'specular', 'voxelpolicies'):
            source = overlay / 'materials' / channel
            if not source.is_dir() or not any(source.rglob('*')):
                raise ValueError(f'Missing authored material channel: {channel}')
            shutil.copytree(source, stage / 'release-overlay/materials' / channel)
        (stage / 'raze').chmod(0o755)
        (stage / 'launch-duke-rt.sh').chmod(0o755)
        inventory = {p.relative_to(stage).as_posix(): sha(p) for p in sorted(stage.rglob('*')) if p.is_file()}
        for name in inventory:
            if Path(name).suffix.lower() in ('.grp', '.kvx', '.bmp', '.exe', '.dll', '.pdb') or '/normalmaps/' in name:
                raise ValueError(f'Unexpected content in public package: {name}')
        (stage / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in inventory.items()))

        def modes(entry):
            entry.uid = entry.gid = 0
            entry.uname = entry.gname = 'root'
            entry.mode = 0o755 if entry.isdir() or Path(entry.name).name in ('raze', 'launch-duke-rt.sh') else 0o644
            return entry

        with tarfile.open(output, 'w:gz') as archive:
            archive.add(stage, arcname=stage.name, filter=modes)
    output.with_suffix(output.suffix + '.sha256').write_text(f'{sha(output)}  {output.name}\n')
    print(f'Created {output} ({output.stat().st_size} bytes)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', type=Path, required=True,
                        help='Compiled Linux executable, libraries, licenses, soundfonts and production shaders')
    parser.add_argument('--product-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.runtime_dir.resolve(), args.product_root.resolve(), args.output.resolve())
