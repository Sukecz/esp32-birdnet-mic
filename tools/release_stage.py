#!/usr/bin/env python3
"""Create and verify a source-bound release snapshot; never overwrite a snapshot."""
import argparse
import contextlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from board_registry import ROOT, boards
from build_receipt import digest, source_digest
from verify_release_inputs import main as verify_inputs


def inventory(directory):
    result = {}
    for name in ('web-flasher', 'manual-ota-firmware'):
        for path in sorted((directory / name).rglob('*')):
            if path.is_symlink():
                raise ValueError('Release snapshots must not contain symlinks')
            if path.is_file():
                result[str(path.relative_to(directory))] = digest(path)
    if not result:
        raise ValueError('Empty release snapshot')
    return result


def verify(directory):
    directory = Path(directory)
    receipt = json.loads((directory / 'release.json').read_text())
    if receipt['schema_version'] != 1 or inventory(directory) != receipt['files']:
        raise ValueError('Release snapshot changed after validation')
    return receipt


def freeze(source, destination, metadata):
    """Transactional local snapshot; existing snapshots must be identical."""
    source, destination = Path(source), Path(destination)
    metadata = {**metadata, 'schema_version': 1, 'files': inventory(source)}
    if destination.exists():
        if verify(destination) != metadata:
            raise ValueError('Refusing to replace a different release snapshot')
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.release-', dir=destination.parent) as tmp:
        stage = Path(tmp) / 'snapshot'
        stage.mkdir()
        for name in ('web-flasher', 'manual-ota-firmware'):
            shutil.copytree(source / name, stage / name)
        (stage / 'release.json').write_text(json.dumps(metadata, indent=2) + '\n')
        verify(stage)
        for path in stage.rglob('*'):
            if path.is_file():
                path.chmod(0o444)
        stage.rename(destination)
    return destination


def create():
    with contextlib.redirect_stdout(sys.stderr):
        verify_inputs()
    if subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT):
        raise ValueError('Commit the reviewed release before creating a publication snapshot')
    receipts = [json.loads((ROOT / 'esp32-birdnet-mic/build' / b['fqbn'].replace(':', '.') / 'build-receipt.json').read_text()) for b in boards()]
    commits = {r.get('source_commit') for r in receipts}
    if len(commits) != 1 or None in commits or any(r.get('working_tree_dirty') is not False for r in receipts):
        raise ValueError('Release builds must share one committed, clean source revision')
    version = json.loads((ROOT / 'web-flasher/manifest.json').read_text())['version']
    source = source_digest()
    publication = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    return freeze(ROOT, ROOT / 'tmp/releases' / f'{version}-{source[:12]}-{publication[:8]}',
                  {'version': version, 'source_sha256': source, 'source_commit': commits.pop(), 'publication_commit': publication})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['create', 'verify'])
    p.add_argument('directory', type=Path, nargs='?')
    args = p.parse_args()
    if args.action == 'create':
        print(create())
    elif args.directory:
        verify(args.directory)
        print('Release snapshot verified')
    else:
        p.error('A snapshot directory is required')


if __name__ == '__main__':
    main()
