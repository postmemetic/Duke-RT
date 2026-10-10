#!/usr/bin/env python3
"""First-run content setup for the native Linux Duke-RT package."""
import argparse
from datetime import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import tempfile
import webbrowser
import zipfile

VOXEL_URL = 'https://www.moddb.com/mods/voxel-duke-nukem-3d/addons/voxel-duke-3d'
VOXEL_ITEMS = {'duke3d.def', 'duke3d_voxels.def', 'duke3d_maphacks.def',
               'readme.txt', 'voxels', 'maphacks'}


def info(message):
    print('[duke-rt] ' + message, flush=True)


def ask(message):
    if not sys.stdin.isatty():
        return ''
    try:
        return input('[duke-rt] ' + message + ' ').strip().strip('"')
    except EOFError:
        return ''


def confirm(message, default=False):
    while True:
        answer = ask(message).lower()
        if not answer:
            return default
        if answer in ('y', 'yes'):
            return True
        if answer in ('n', 'no'):
            return False
        info('Please answer yes or no.')


def path_value(value):
    return Path(os.path.expandvars(str(value))).expanduser().resolve()


def child_case(directory, name):
    if not directory.is_dir():
        return directory / name
    matches = [p for p in directory.iterdir() if p.name.lower() == name.lower()]
    if len(matches) > 1:
        raise ValueError(f'Ambiguous filename {name} in {directory}')
    return matches[0] if matches else directory / name


def validate_grp(value):
    path = path_value(value)
    if path.is_dir():
        path = child_case(path, 'DUKE3D.GRP')
    with path.open('rb') as stream:
        header = stream.read(16)
        if len(header) != 16 or header[:12] != b'KenSilverman':
            raise ValueError(f'Not a valid Build GRP: {path}')
        count = struct.unpack('<I', header[12:])[0]
        size = path.stat().st_size
        if not 1 <= count <= 100000 or 16 + count * 16 > size:
            raise ValueError(f'Invalid GRP directory: {path}')
        entries = [stream.read(16) for _ in range(count)]
        names = {e[:12].split(b'\0', 1)[0].upper() for e in entries}
        if b'GAME.CON' not in names or b'PALETTE.DAT' not in names:
            raise ValueError(f'GRP does not contain Duke game data: {path}')
        if 16 + count * 16 + sum(struct.unpack('<I', e[12:])[0] for e in entries) != size:
            raise ValueError(f'Truncated or inconsistent GRP: {path}')
    return path


def read_vdf(path):
    """Read Steam's quoted KeyValues subset, including nested library records."""
    tokens = iter(re.findall(r'"(?:\\.|[^"\\])*"|[{}]', path.read_text(encoding='utf-8-sig')))

    def block():
        result = {}
        for token in tokens:
            if token == '}':
                return result
            key = json.loads(token)
            value = next(tokens)
            result[key] = block() if value == '{' else json.loads(value)
        return result

    return block()


def steam_candidates():
    home = Path.home()
    roots = [home / '.steam/steam', home / '.steam/root',
             Path(os.environ.get('XDG_DATA_HOME', home / '.local/share')) / 'Steam',
             home / '.var/app/com.valvesoftware.Steam/data/Steam']
    libraries = set()
    for root in roots:
        if not root.is_dir():
            continue
        libraries.add(root.resolve())
        try:
            values = read_vdf(root / 'steamapps/libraryfolders.vdf')['libraryfolders'].values()
            for value in values:
                location = value.get('path') if isinstance(value, dict) else value
                if location and Path(location).is_absolute():
                    libraries.add(Path(location))
        except (OSError, ValueError, KeyError, StopIteration):
            pass
    for library in sorted(libraries):
        common = library / 'steamapps/common'
        try:
            install = read_vdf(library / 'steamapps/appmanifest_434050.acf')['AppState']['installdir']
            candidate = (common / install).resolve()
            if candidate.is_relative_to(common.resolve()):
                yield candidate
        except (OSError, ValueError, KeyError, StopIteration):
            pass
        yield common / 'Duke Nukem 3D Twentieth Anniversary World Tour'
        yield common / 'Duke Nukem 3D/gameroot'


def resolve_grp(explicit, saved, root):
    if explicit:
        return validate_grp(explicit)
    if saved:
        try:
            return validate_grp(saved)
        except (OSError, ValueError) as error:
            info(f'Saved game path is unavailable: {error}')
    seen = set()
    for candidate in [root / 'game', root, *steam_candidates()]:
        try:
            grp = validate_grp(candidate)
        except (OSError, ValueError):
            continue
        if grp in seen:
            continue
        seen.add(grp)
        info(f'Found Duke game data: {grp}')
        if not sys.stdin.isatty() or confirm('Use this game data? [Y/n]', default=True):
            return grp
    selected = ask('Enter the DUKE3D.GRP file or installation folder (blank cancels):')
    if selected:
        return validate_grp(selected)
    raise ValueError('DUKE3D.GRP is required. Run with --game-root "/path/to/game or DUKE3D.GRP".')


def atomic_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def installed(directory):
    try:
        manifest = json.loads((directory / 'import.json').read_text())
        files = manifest['files']
        return bool(files) and all((directory / p).is_file() for p in files)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def commit_import(stage, target, source, credit):
    files = {p.relative_to(stage).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(stage.rglob('*')) if p.is_file()}
    atomic_json(stage / 'import.json', {'source': str(source), 'credit': credit, 'files': files})
    backup = target.with_name(target.name + '.previous')
    if target.is_symlink() or backup.is_symlink():
        raise ValueError('Refusing to replace a symlinked content directory')
    if backup.exists():
        raise ValueError(f'Previous interrupted import remains at {backup}; preserve or move it before retrying')
    if target.exists():
        target.rename(backup)
    try:
        stage.rename(target)
    except OSError:
        if backup.exists():
            backup.rename(target)
        raise
    if backup.exists():
        shutil.rmtree(backup)
    return len(files)


def import_normals(source, target):
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError('Normals require Pillow (Ubuntu: install python3-pil); other content can still run.') from error
    sources = sorted(p for p in source.rglob('*') if p.is_file()
                     and re.fullmatch(r'TILES\d+', p.parent.name, re.I)
                     and re.fullmatch(r'\d+_n\.bmp', p.name, re.I))
    if not sources:
        raise ValueError(f'No World Tour TILES*/<number>_n.bmp normals found at {source}')
    with tempfile.TemporaryDirectory(prefix='normals-', dir=target.parent) as temporary:
        stage = Path(temporary) / 'payload'
        destination = stage / 'materials/normalmaps/auto'
        destination.mkdir(parents=True)
        for source_file in sources:
            tile = int(source_file.stem.split('_')[0])
            output = destination / f'#{tile:05d}.png'
            if output.exists():
                raise ValueError(f'Duplicate normal map for tile {tile}')
            with Image.open(source_file) as image:
                image.convert('RGB').save(output)
        return commit_import(stage, target, source, 'World Tour — Nerve Software / Gearbox; user-owned local data')


def import_voxels(archive_path, target):
    with zipfile.ZipFile(archive_path) as archive:
        entries = archive.infolist()
        if len(entries) > 20000 or sum(e.file_size for e in entries) > 1024 ** 3:
            raise ValueError('Voxel archive exceeds the supported pack size')
        seen = set()
        for entry in entries:
            path = PurePosixPath(entry.filename)
            mode = entry.external_attr >> 16
            if (path.is_absolute() or '..' in path.parts or '\\' in entry.filename or ':' in entry.filename
                    or stat.S_ISLNK(mode) or path.as_posix().casefold() in seen):
                raise ValueError(f'Unsafe or ambiguous ZIP entry: {entry.filename}')
            seen.add(path.as_posix().casefold())
        roots = [PurePosixPath(e.filename).parent for e in entries if PurePosixPath(e.filename).name == 'duke3d.def']
        names = {e.filename for e in entries}
        roots = [p for p in roots if all(str(p / n) in names for n in ('duke3d_voxels.def', 'duke3d_maphacks.def'))]
        if len(roots) != 1:
            raise ValueError('Select the Voxel Duke 3D ZIP containing duke3d.def, voxel/maphack DEFs and their data.')
        pack = roots[0]
        with tempfile.TemporaryDirectory(prefix='voxels-', dir=target.parent) as temporary:
            stage = Path(temporary) / 'payload'
            stage.mkdir()
            for entry in entries:
                path = PurePosixPath(entry.filename)
                if not path.is_relative_to(pack):
                    continue
                relative = path.relative_to(pack)
                if not relative.parts or relative.parts[0] not in VOXEL_ITEMS or entry.is_dir():
                    continue
                output = stage.joinpath(*relative.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as src, output.open('wb') as dest:
                    shutil.copyfileobj(src, dest)
            for required in ('voxels', 'maphacks'):
                if not (stage / required).is_dir() or not any((stage / required).rglob('*')):
                    raise ValueError(f'Voxel pack is missing {required} data')
            return commit_import(stage, target, archive_path, 'Voxel Duke 3D — Daniel Peterson (Cheello / _chillo); ' + VOXEL_URL)


def choose_optional(name, requested, state):
    if requested is not None:
        return requested == 'yes' if requested != 'ask' else confirm(f'Enable optional {name}? [y/N]')
    if name in state:
        return state[name]
    return confirm(f'Enable optional {name}? [y/N]')


def setup(args, root, data, state):
    grp = resolve_grp(args.game_root, state.get('grp'), root)
    state['grp'] = str(grp)
    content = data / 'content'
    content.mkdir(exist_ok=True)
    overlays = []
    for provider in ('voxels', 'normals'):
        requested = getattr(args, provider)
        if provider == 'voxels' and args.voxel_zip and requested is None:
            requested = 'yes'
        enabled = choose_optional(provider, requested, state)
        target = content / provider
        try:
            refresh = args.refresh or (args.voxel_zip if provider == 'voxels' else args.normal_source)
            if enabled and (refresh or not installed(target)):
                if provider == 'normals':
                    source = path_value(args.normal_source) if args.normal_source else child_case(grp.parent, 'textures')
                    count = import_normals(source, target)
                    info(f'Imported {count} World Tour normal maps. Credit: Nerve Software / Gearbox.')
                else:
                    archive = path_value(args.voxel_zip) if args.voxel_zip else root / 'voxel_duke3d.zip'
                    if not archive.is_file():
                        info('Voxel Duke 3D by Daniel Peterson (Cheello / _chillo): ' + VOXEL_URL)
                        if sys.stdin.isatty() and confirm('Open the download page in your browser? [Y/n]', default=True):
                            if not webbrowser.open(VOXEL_URL):
                                info('Browser could not be opened. Open the URL above manually.')
                        selected = ask('After downloading, enter the voxel ZIP path (blank skips):')
                        if not selected:
                            raise ValueError('No voxel archive selected')
                        archive = path_value(selected)
                    count = import_voxels(archive, target)
                    info(f'Imported {count} voxel-pack files; original readme retained. Credit: Cheello / Daniel Peterson.')
            if enabled:
                overlays.append(target)
        except (OSError, ValueError, zipfile.BadZipFile, RuntimeError, webbrowser.Error) as error:
            info(f'Optional {provider} unavailable: {error}. Continuing without {provider}.')
            enabled = False
        state[provider] = enabled
        info(f'{provider}: {"enabled" if enabled else "disabled"}')
    overlays.append(root / 'release-overlay')
    return grp, overlays


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog='Arguments after -- are passed to the game unchanged.')
    parser.add_argument('--launch-root', type=path_value, required=True)
    parser.add_argument('--game-root', '-GameRoot', '-gamegrp', help='Game directory or DUKE3D.GRP file')
    parser.add_argument('--normal-source', '-SourceRoot', help='Optional World Tour textures directory')
    parser.add_argument('--normals', choices=('yes', 'no', 'ask'))
    parser.add_argument('--voxels', choices=('yes', 'no', 'ask'))
    parser.add_argument('--voxel-zip', '-VoxelZip')
    parser.add_argument('--refresh', action='store_true', help='Reimport enabled optional content')
    parser.add_argument('--setup-only', action='store_true')
    values = sys.argv[1:]
    split = values.index('--') if '--' in values else len(values)
    args = parser.parse_args(values[:split])
    game_args = values[split + 1:]
    root = args.launch_root
    try:
        if not (root / 'raze').is_file() or not (root / 'release-overlay/LIGHTOVR').is_file():
            raise ValueError('Incomplete package: raze and release-overlay/LIGHTOVR must be beside the launcher.')
        data = root / 'userdata'
        for name in ('', 'logs', 'saves', 'screenshots'):
            (data / name).mkdir(exist_ok=True)
        state_file = data / 'content-preferences.json'
        with (data / 'setup.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                state = json.loads(state_file.read_text()) if state_file.exists() else {}
                if not isinstance(state, dict):
                    raise ValueError('Expected a JSON object')
            except (ValueError, OSError) as error:
                raise ValueError(f'Cannot read {state_file}: {error}; preserve it and move it aside to reset setup') from error
            grp, overlays = setup(args, root, data, state)
            atomic_json(state_file, state)
            config = data / 'raze.ini'
            if not config.exists():
                shutil.copyfile(root / 'defaults/raze.ini', config)
        log = data / 'logs' / (datetime.now().strftime('session-%Y%m%d-%H%M%S-') + str(os.getpid()) + '.log')
        command = [str(root / 'raze'), '-gamegrp', str(grp), '-config', str(config),
                   '-savedir', str(data / 'saves'), '-shotdir', str(data / 'screenshots'),
                   '-file', *map(str, overlays), '+logfile', str(log), *game_args]
        atomic_json(data / 'last-launch.json', {'grp': str(grp), 'overlays': list(map(str, overlays)), 'argv': command})
        info(f'Game data: {grp}')
        for overlay in overlays:
            info(f'Mount: {overlay}')
        if args.setup_only:
            return 0
        info(f'Session log: {log}')
        os.chdir(root)
        os.execv(command[0], command)
    except (OSError, ValueError) as error:
        info(str(error))
        return 1


if __name__ == '__main__':
    sys.exit(main())
