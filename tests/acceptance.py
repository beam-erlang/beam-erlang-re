#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent installed package, conformance, linkage and lifetime acceptance."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import tomllib


def source_identity(source):
    files = {}
    excluded = {'.git', 'build', 'install', '.beam', '__pycache__'}
    for directory, children, names in os.walk(source):
        children[:] = sorted(name for name in children if name not in excluded)
        for name in sorted(names):
            if name == 'beam.lock':
                continue
            path = Path(directory) / name
            if path.is_file():
                files[str(path.relative_to(source))] = hashlib.sha256(path.read_bytes()).hexdigest()
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'files': files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('sdk', 'source', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cmake', default='/usr/bin/cmake')
    parser.add_argument('--embedding-only', action='store_true', help='Focused native checks without package/conformance qualification')
    parser.add_argument('--case', action='append', default=[], help='Focused original-suite case; may repeat, omits package workflow qualification')
    parser.add_argument('--link', choices=['Runtime/MODULE', 'Runtime/STATIC', 'RuntimeStatic/STATIC'], help='Focused linkage selection')
    args = parser.parse_args()
    args.sdk = args.sdk.resolve(); args.source = args.source.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    result = {'sdk': tomllib.loads((args.sdk / 'lib/beam/sdk.toml').read_text()),
              'upstream': (args.source / 'UPSTREAM_COMMIT').read_text().strip(),
              'source': source_identity(args.source),
              'native-sha256': hashlib.sha256((args.source / 'native/re_nif.c').read_bytes()).hexdigest(),
              'focused': args.embedding_only or bool(args.case) or bool(args.link), 'selected-cases': args.case,
              'checks': [], 'conformance': {}}
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('BEAM_', 'OTP_', 'ERL_', 'LD_', 'DYLD_'))}
    env.update(LANG='C.UTF-8', LC_ALL='C.UTF-8')
    failure = None
    def passed(label):
        result['checks'].append(label)
        print(label + ': passed', flush=True)
    try:
        with tempfile.TemporaryDirectory(prefix='run-', dir=args.work) as temporary:
            root = Path(temporary).resolve()
            def run(command, *, input=None, success=True, cwd=None, timeout=180):
                process = subprocess.Popen(list(map(str, command)), cwd=cwd or root, env=env,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, start_new_session=True)
                try:
                    stdout, stderr = process.communicate(input=input, timeout=timeout)
                except BaseException:
                    os.killpg(process.pid, signal.SIGKILL); process.communicate(); raise
                if (process.returncode == 0) != success:
                    raise RuntimeError(f'{command}: status {process.returncode}\n{stdout[-9000:]}\n{stderr[-9000:]}')
                return stdout, stderr
            sdk = root / 'SDK 日本'
            shutil.copytree(args.sdk, sdk, symlinks=True)
            source = root / 'package'
            shutil.copytree(args.source, source, ignore=shutil.ignore_patterns(
                '.git', 'build', 'install', '.beam', 'beam.lock', '__pycache__'))
            fixture = root / 'fixtures'; fixture.mkdir()
            beam = sdk / 'bin/beam'; pkg = sdk / 'bin/beam-pkg'
            sources = [source / p for p in ('src/re.erl', 'src/beam_re_errors.erl',
                                            'tests/re_checks.erl', 'tests/beam_re_test.erl', 'tests/erl_tracer.erl',
                                            'tests/import_bounds_checks.erl')]
            print('Compiling explicitly selected modern fixture modules', flush=True)
            run([beam, 'compile', '-o', fixture, '--', *sources], timeout=600)
            modern_paths=sorted(fixture.glob('*.beam'))
            canonical_paths=[]
            if not args.embedding_only:
                prepared = fixture / 'conformance'
                run(['/usr/bin/python3', source / 'tests/conformance/prepare.py', '--output', prepared, '--sdk', sdk])
                manifest = json.loads((prepared / 'manifest.json').read_text())
                include = [item for p in manifest.get('include_directories', []) for item in ('-I', p)]
                conformance_ebin=prepared/'ebin'
                run([beam,'compile','--native-bindings=canonical',*include,'-o',conformance_ebin,'--',*manifest['sources']],timeout=600)
                run([beam,'compile','--native-bindings=canonical','-DRE_CANONICAL_PROFILE','-o',conformance_ebin,'--',*sources],timeout=600)
                canonical_paths=sorted(conformance_ebin.glob('*.beam'))
            header = []
            embedded={}
            for path in modern_paths+canonical_paths:
                if path in embedded: continue
                n=len(embedded)
                data = path.read_bytes()
                header.append(f'static const unsigned char module_{n}[] = {{' + ','.join(map(str, data)) + '};\n')
                embedded[path]=f'{{"{path.stem}",{{module_{n},sizeof(module_{n})}},"embedded acceptance fixture"}}'
            for profile,paths in [('modern',modern_paths),('canonical',canonical_paths or modern_paths)]:
                header.append(f'static const beam_module_spec fixture_{profile}_modules[] = {{' + ',\n'.join(embedded[p] for p in paths) + '};\n')
                header.append(f'static const size_t fixture_{profile}_count=sizeof(fixture_{profile}_modules)/sizeof(fixture_{profile}_modules[0]);\n')
            (fixture / 'fixture.h').write_text(''.join(header))
            if not args.embedding_only and not args.case:
                project = root / 'project'; project.mkdir()
                shutil.copy2(source / 'tests/re_checks.erl', project)
                (project / 're_consumer.erl').write_text('''-module(re_consumer).
-export([main/1]).
main(_) -> ok=re_checks:run(), io:put_chars("RE_PACKAGE_OK\\n"), ok.
''')
                manifest = '''format=1
[package]
name="test/re-consumer"
version="1"
[build]
sources=["re_consumer.erl","re_checks.erl"]
[dependencies]
"erlangsters/re"={version="0.0.1",path="../package"}
[host]
modules=["erlang","io","beam_persistent_term"]
[entry]
module="re_consumer"
'''
                (project / 'beam.toml').write_text(manifest)
                stdout, stderr = run([beam, 'eval', 're:run(<<"a">>,<<"a">>).'], success=False)
                assert stderr and 'RE_PACKAGE_OK' not in stdout
                passed('optional-default-profile')
                run([beam, 'build', project], timeout=300); run([pkg, 'install', project])
                stdout, stderr = run([beam, 'run', '--project', project])
                assert stdout == 'RE_PACKAGE_OK\n' and not stderr, (stdout, stderr)
                passed('ordinary-package-dependency')
                stdout, stderr = run([beam, 'shell', '--project', project], input='re_checks:run().\nq().\n')
                assert 'ok' in stdout and not stderr, (stdout, stderr)
                passed('installed-project-shell')
                native, = (project / '.beam').rglob('re.so')
                symbols = subprocess.check_output(['/usr/bin/nm', '-D', '--defined-only', str(native)], text=True)
                exported = {line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3}
                assert exported == {'nif_init', 'beam_nif_sdk_v1'}, exported
                passed('production-symbol-privacy')
                original = native.read_bytes()
                for label, changed in [('missing-native', None), ('wrong-sdk', original.replace(result['sdk']['build-id'].encode(), b'0' * 64))]:
                    if changed is None: native.unlink()
                    else:
                        assert changed != original
                        native.write_bytes(changed)
                    try:
                        stdout, stderr = run([beam, 'run', '--project', project], success=False)
                        assert stderr and 'RE_PACKAGE_OK' not in stdout
                    finally: native.write_bytes(original)
                    stdout, stderr = run([beam, 'run', '--project', project])
                    assert stdout == 'RE_PACKAGE_OK\n' and not stderr
                    passed(label + '-recovery')
                collision = root / 'collision'; collision.mkdir()
                (collision / 're.erl').write_text('-module(re).\n-export([conflict/0]).\nconflict()->ok.\n')
                (collision / 'beam.toml').write_text('format=1\n[package]\nname="test/collision"\nversion="1"\n[build]\nsources=["re.erl"]\n')
                lock = (project / 'beam.lock').read_bytes()
                (project / 'beam.toml').write_text(manifest.replace('[host]', '"test/collision"={version="1",path="../collision"}\n[host]'))
                _, stderr = run([beam, 'build', project], success=False)
                assert 'module' in stderr.lower() and (project / 'beam.lock').read_bytes() == lock
                (project / 'beam.toml').write_text(manifest)
                passed('duplicate-provider-rejection')
            for runtime, kind in [('Runtime', 'MODULE'), ('Runtime', 'STATIC'), ('RuntimeStatic', 'STATIC')]:
                if args.link and args.link != runtime + '/' + kind:
                    continue
                build = root / (runtime + '-' + kind)
                run([args.cmake, '-S', source / 'tests/embedding', '-B', build, '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={sdk}',
                     f'-DPACKAGE_SOURCE={source}', f'-DFIXTURE_DIRECTORY={fixture}',
                     f'-DRUNTIME_COMPONENT={runtime}', f'-DBEAM_RE_NATIVE_KIND={kind}'])
                run([args.cmake, '--build', build, '-j', '4'])
                if kind == 'STATIC':
                    symbols = subprocess.check_output(['/usr/bin/nm', '--defined-only', '--extern-only', str(build / 'native/re.a')], text=True)
                    defined = {line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3}
                    unexpected = {n for n in defined if not n.startswith(('beam_re_backend_', 'beam_re_test_', 'beam_package_'))}
                    assert not unexpected, sorted(unexpected)
                bundle = root / ('bundle-' + runtime + '-' + kind)
                (bundle / 'bin').mkdir(parents=True); (bundle / 'lib').mkdir()
                shutil.copy2(build / 're_host', bundle / 'bin')
                if runtime == 'Runtime': shutil.copy2(sdk / 'lib/libbeam.so', bundle / 'lib')
                command = [bundle / 'bin/re_host']
                if kind == 'MODULE':
                    shutil.copy2(build / 'native/re.so', bundle / 'lib')
                    command.append(bundle / 'lib/re.so')
                moved = bundle.with_name(bundle.name + ' relocated 日本'); bundle.rename(moved)
                command = [str(x).replace(str(bundle), str(moved)) for x in command]
                stdout, stderr = run(command)
                assert stdout.endswith('RE_EMBEDDING_OK\n') and not stderr, (stdout[-4000:], stderr)
                passed(runtime + '/' + kind + '-lifetime-relocation-coexistence')
                if not args.embedding_only:
                    for case in args.case or [None]:
                        suite_command = command + (['--case', source / 'tests/upstream/re_SUITE_data', case] if case
                            else ['--suite', source / 'tests/upstream/re_SUITE_data'])
                        print(runtime + '/' + kind + ': running ' + (case or 'complete upstream conformance'), flush=True)
                        stdout, stderr = run(suite_command, timeout=1200)
                        assert stdout.endswith('RE_EMBEDDING_OK\n') and not stderr, (stdout[-9000:], stderr[-9000:])
                        rows = [{'case': match.group(1), 'elapsed-us': int(match.group(2)), 'status': 'passed'}
                                for match in re.finditer(r'RE_CASE (\w+) pass \((\d+) us\)', stdout)]
                        result['conformance'][runtime + '/' + kind + (':' + case if case else '')] = rows
                        passed(runtime + '/' + kind + '-upstream-' + (case or 'conformance'))
                isolate = ['/usr/bin/bwrap', '--die-with-parent', '--unshare-net', '--ro-bind', '/', '/',
                           '--dev', '/dev', '--proc', '/proc']
                for hidden in (sdk, args.sdk, source, args.source, build, fixture):
                    isolate += ['--tmpfs', str(hidden)]
                stdout, stderr = run(isolate + ['--chdir', str(root), '--'] + command)
                assert stdout.endswith('RE_EMBEDDING_OK\n') and not stderr, (stdout, stderr)
                passed(runtime + '/' + kind + '-isolated')
                shutil.rmtree(moved); shutil.rmtree(build)
    except BaseException as exc:
        failure = exc
        result['failure'] = str(exc)[-12000:]
    finally:
        (args.work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    if failure is not None:
        raise failure
    print('RE installed acceptance passed', flush=True)


if __name__ == '__main__':
    main()
