#!/usr/bin/env python3
"""Verify pinned conformance inputs and prepare narrowly adapted fixture sources."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    sdk = args.sdk.resolve()
    if not (sdk / 'bin/beam').is_file():
        parser.error('select an installed SDK containing its own beam command')
    sources = output / 'src'
    sources.mkdir(parents=True, exist_ok=True)
    (output / 'ebin').mkdir(exist_ok=True)
    support = json.loads((root / 'support-imports.json').read_text())
    adaptations = json.loads((root / 'adaptations.json').read_text())
    if support['upstream_commit'] != adaptations['upstream_commit']:
        raise SystemExit('conformance source pins do not match')
    inputs = []
    for entry in support['files']:
        path = root / entry['path']
        if digest(path) != entry['sha256']:
            raise SystemExit(f'changed pinned test support: {entry["path"]}')
        target = sources / path.name
        shutil.copyfile(path, target)
        inputs.append({'path': entry['path'], 'sha256': entry['sha256']})
    for entry in adaptations['files']:
        path = root / entry['source']
        if digest(path) != entry['sha256']:
            raise SystemExit(f'changed upstream test input: {entry["source"]}')
        # Literal carriage returns occur inside generated quoted fixtures.
        # Text-mode universal-newline conversion would change their subjects
        # while leaving escaped regular-expression tokens unchanged.
        text = path.read_bytes().decode('utf-8')
        for change in entry['replacements']:
            actual = text.count(change['old'])
            if actual != change['count'] or not actual:
                raise SystemExit(f'adaptation count mismatch in {path.name}: {change["old"]!r}')
            text = text.replace(change['old'], change['new'])
        (sources / entry['output']).write_bytes(text.encode('utf-8'))
        inputs.append({'path': entry['source'], 'sha256': entry['sha256']})
    for path in root.glob('*.erl'):
        shutil.copyfile(path, sources / path.name)
        inputs.append({'path': path.name, 'sha256': digest(path)})
    source_files = sorted(sources.glob('*.erl'))
    inventory = {
        'format': 1,
        'upstream_commit': adaptations['upstream_commit'],
        'sdk': str(sdk),
        'sources': [str(path) for path in source_files],
        'include_directories': [str(sources)],
        'modules': [path.stem for path in source_files],
        'inputs': inputs,
        'adaptations_sha256': digest(root / 'adaptations.json'),
        'support_imports_sha256': digest(root / 'support-imports.json'),
        'prepared_sources': {path.name: digest(path) for path in source_files},
        'compiler_argv': [str(sdk / 'bin/beam'), 'compile', '-I', str(sources),
                          '-o', str(output / 'ebin'), '--',
                          *[str(path) for path in source_files]],
    }
    (output / 'manifest.json').write_text(json.dumps(inventory, indent=2) + '\n')
    print(json.dumps({'manifest': str(output / 'manifest.json'),
                      'modules': inventory['modules']}))


if __name__ == '__main__':
    main()
