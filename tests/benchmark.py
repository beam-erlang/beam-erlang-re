#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure matched installed BIF and independent NIF regex implementations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import tomllib


CASES = {'compile': 200, 'compiled': 5000, 'implicit': 500, 'large': 10,
         'unicode': 10, 'backtrack': 30, 'global': 20, 'captures': 100}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def spread(values):
    return {'median': statistics.median(values), 'minimum': min(values),
            'maximum': max(values), 'p05': percentile(values, .05),
            'p95': percentile(values, .95)}


def sdk_info(path):
    configs = list((path / 'lib').rglob('BEAMConfig.cmake'))
    if len(configs) != 1:
        raise ValueError(f'exactly one installed BEAMConfig.cmake required: {path}')
    match = re.search(r'set\(BEAM_SDK_HAS_LEGACY_RE_BIFS\s+"?(ON|OFF)"?\s*\)',
                      configs[0].read_text())
    if match is None:
        raise ValueError(f'SDK does not declare legacy regex registration: {path}')
    headers = ['include/beam/native.h', 'include/beam/zlib.h', 'include/beam/nif/erl_nif.h']
    return {'path': str(path),
            'sdk': tomllib.loads((path / 'lib/beam/sdk.toml').read_text()),
            'target': tomllib.loads((path / 'lib/beam/target.toml').read_text()),
            'legacy_re_bifs': match[1] == 'ON',
            'native_headers': {name: digest(path / name) for name in headers}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline-sdk', 'candidate-sdk', 'source', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--warmups', type=int, default=2)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    parser.add_argument('--producer-source-id', help='Caller-attested matching runtime source snapshot digest')
    parser.add_argument('--baseline-source-id', help='Caller-attested legacy runtime source snapshot digest')
    parser.add_argument('--candidate-source-id', help='Caller-attested candidate runtime source snapshot digest')
    parser.add_argument('--cmake', default='/usr/bin/cmake')
    args = parser.parse_args()
    if bool(args.baseline_source_id) != bool(args.candidate_source_id):
        parser.error('baseline and candidate source identities must be supplied together')
    if args.producer_source_id and args.baseline_source_id:
        parser.error('select matching-source or separate-source provenance')
    if args.samples < 1 or args.samples > 30 or args.warmups < 0 or args.warmups > 20:
        parser.error('samples must be 1..30 and warmups 0..20')
    args.source = args.source.resolve()
    args.work = args.work.resolve()
    sdks = {'baseline': args.baseline_sdk.resolve(), 'candidate': args.candidate_sdk.resolve()}
    if any(args.work.is_relative_to(sdk) for sdk in sdks.values()):
        parser.error('work must be outside selected immutable SDK installations')
    if args.work == args.source or (args.work.is_relative_to(args.source) and
                                   args.work.relative_to(args.source).parts[0] != 'build'):
        parser.error('package-local work must be under ignored build/')
    args.work.mkdir(parents=True, exist_ok=True)
    info = {kind: sdk_info(sdk) for kind, sdk in sdks.items()}
    if not info['baseline']['legacy_re_bifs'] or info['candidate']['legacy_re_bifs']:
        parser.error('baseline requires legacy re BIFs ON and candidate requires OFF')
    comparable = ['system', 'architecture', 'processor', 'abi', 'libc',
                  'compiler-environment', 'native-compiler-version', 'flavor',
                  'baseline', 'apple-variant']
    for key in comparable:
        if info['baseline']['target'].get(key) != info['candidate']['target'].get(key):
            parser.error(f'SDK target/compiler mismatch: {key}')
    if info['baseline']['native_headers'] != info['candidate']['native_headers']:
        parser.error('SDK public native headers differ')
    imported = json.loads((args.source / 'tests/benchmark/imports.json').read_text())
    for entry in imported['files']:
        path = args.source / 'tests/benchmark' / entry['destination']
        if digest(path) != entry['sha256']:
            parser.error(f'changed pristine baseline input: {entry["destination"]}')
    support = json.loads((args.source / 'tests/conformance/support-imports.json').read_text())
    lists_entry, = [entry for entry in support['files'] if entry['path'] == 'support/lists.erl']
    lists_source = args.source / 'tests/conformance/support/lists.erl'
    if digest(lists_source) != lists_entry['sha256']:
        parser.error('changed source-pinned baseline lists support')
    identity_files = ['UPSTREAM_COMMIT', 'CMakeLists.txt', 'tools/generate_backend.py',
                      'src/re.erl', 'src/beam_re_errors.erl',
                      'native/re_nif.c', 'vendor/pcre2/pcre2_match.c',
                      'tests/benchmark.py',
                      'tests/benchmark/re_bench.erl', 'tests/benchmark/main.c',
                      'tests/builtin_bindings.h',
                      'tests/benchmark/CMakeLists.txt', 'tests/benchmark/imports.json']
    source_ids = {'baseline': args.baseline_source_id or args.producer_source_id,
                  'candidate': args.candidate_source_id or args.producer_source_id}
    equivalence = ('not independently attested' if not source_ids['baseline'] else
                   'caller-attested matching sources' if source_ids['baseline'] == source_ids['candidate']
                   else 'distinct caller-attested sources')
    result = {'format': 1, 'sdk_inputs': info, 'upstream_commit': imported['commit'],
              'producer_source_id': args.producer_source_id,
              'producer_source_ids': source_ids,
              'producer_source_equivalence': equivalence,
              'source_sha256': {name: digest(args.source / name) for name in identity_files},
              'samples_per_case': args.samples, 'warmups_per_process': args.warmups,
              'scheduling': {'normal': 1, 'dirty_cpu': 1, 'dirty_io': 1},
              'method': {'clock': 'CLOCK_PROCESS_CPUTIME_ID and CLOCK_MONOTONIC around one warmed VM call',
                         'process_order': 'alternate baseline/candidate for every case and sample',
                         'latency': 'gaps between competing Erlang process executions; first 16384 gaps retained per sample, maximum over all gaps',
                         'candidate_instrumentation': 'backend memory counters enabled; DEBUG/yield coverage disabled',
                         'backend_volume': 'successful backend alloc/realloc requests during uncontended sample; requested bytes include full new realloc size',
                         'backend_retention': 'outstanding bytes/allocations/resources after uncontended sample and explicit Erlang GC, with prepared inputs live; pending reclamation may remain',
                         'backend_peak': 'whole process, including preparation, warmups and contended sample',
                         'backend_exclusions': 'allocator headers, NIF resource payloads, owned environments, subject flattening and ordinary Erlang storage',
                         'baseline_backend_counters': 'unavailable; null is not zero',
                         'performance_threshold': None},
              'cases': {}, 'completed': False}
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('BEAM_', 'OTP_', 'ERL_', 'LD_', 'DYLD_'))}
    env.update(LANG='C.UTF-8', LC_ALL='C.UTF-8')

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)

    def run(command, cwd, timeout=180):
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            raise
        if process.returncode:
            raise RuntimeError(f'{command}: status {process.returncode}\n{stdout[-2000:]}\n{stderr[-7000:]}')
        return stdout

    failure = None
    try:
        with tempfile.TemporaryDirectory(prefix='run-', dir=args.work) as temporary:
            root = Path(temporary)
            source = root / 'package'
            shutil.copytree(args.source, source, ignore=shutil.ignore_patterns(
                '.git', 'build', 'install', '.beam', '__pycache__', 'beam.lock'))
            for name, expected in result['source_sha256'].items():
                if digest(source / name) != expected:
                    raise RuntimeError(f'package source changed while preparing benchmark: {name}')
            hosts = {}
            for kind, sdk in sdks.items():
                fixture = root / (kind + '-fixture'); fixture.mkdir()
                sources = [source / 'tests/benchmark/re_bench.erl']
                if kind == 'baseline':
                    sources += [source / 'tests/benchmark/original/re.erl',
                                source / 'tests/conformance/support/lists.erl']
                else:
                    sources += [source / 'src/re.erl', source / 'src/beam_re_errors.erl',
                                source / 'tests/conformance/beam_unicode.erl',
                                source / 'tests/conformance/beam_binary.erl']
                run([sdk / 'bin/beam', 'compile', '--deterministic', '-o', fixture, '--', *sources], root, 300)
                definitions, inventory = [], []
                for index, path in enumerate(sorted(fixture.glob('*.beam'))):
                    data = path.read_bytes()
                    definitions.append(f'static const unsigned char module_{index}[]={{' + ','.join(map(str, data)) + '};\n')
                    inventory.append(f'{{"{path.stem}",{{module_{index},sizeof(module_{index})}},"source-pinned benchmark fixture"}}')
                definitions.append('static const beam_module_spec fixture_modules[]={' + ','.join(inventory) + '};\n')
                definitions.append('static const size_t fixture_module_count=sizeof(fixture_modules)/sizeof(fixture_modules[0]);\n')
                (fixture / 'fixture.h').write_text(''.join(definitions))
                build = root / (kind + '-build')
                run([args.cmake, '-S', source / 'tests/benchmark', '-B', build, '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=/usr/bin/cc',
                     f'-DCMAKE_PREFIX_PATH={sdk}', f'-DPACKAGE_SOURCE={source}',
                     f'-DFIXTURE_DIRECTORY={fixture}',
                     '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
                     '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
                     f'-DRE_BENCH_CANDIDATE={"ON" if kind == "candidate" else "OFF"}'], root)
                run([args.cmake, '--build', build, '-j', '4'], root, 300)
                command = [build / 're_benchmark']
                if kind == 'candidate':
                    command += [build / 'native/re.so']
                hosts[kind] = command
            for case in args.cases:
                samples = {kind: [] for kind in sdks}
                latencies = {kind: [] for kind in sdks}
                for index in range(args.samples):
                    order = ('baseline', 'candidate') if index % 2 == 0 else ('candidate', 'baseline')
                    for kind in order:
                        command = hosts[kind] + [case, str(CASES[case]), str(args.warmups)]
                        sample = json.loads(run(command, root, 180))
                        if sample['sdk_build_id'] != info[kind]['sdk']['build-id']:
                            raise RuntimeError('executed runtime differs from selected SDK')
                        latency = sample['latency']
                        values = latency.pop('samples_ns')
                        latencies[kind].extend(values)
                        latency['sampled_gaps'] = len(values)
                        latency['p50_ns'] = percentile(values, .50)
                        latency['p95_ns'] = percentile(values, .95)
                        latency['p99_ns'] = percentile(values, .99)
                        samples[kind].append(sample)
                report = {'iterations_per_sample': CASES[case], 'variants': {}}
                for kind, measurements in samples.items():
                    metrics = {}
                    for field in ('cpu_ns', 'wall_ns', 'contended_cpu_ns', 'contended_wall_ns'):
                        metrics[field + '_per_operation'] = spread([m[field] / CASES[case] for m in measurements])
                    metrics['latency_ns'] = {'p50': percentile(latencies[kind], .50),
                                             'p95': percentile(latencies[kind], .95),
                                             'p99': percentile(latencies[kind], .99),
                                             'maximum': max(m['latency']['max_ns'] for m in measurements),
                                             'sampled_gaps': len(latencies[kind]),
                                             'observations': sum(m['latency']['observations'] for m in measurements)}
                    for field in ('backend_allocations', 'backend_allocated_bytes'):
                        values = [m[field] / CASES[case] for m in measurements if m[field] is not None]
                        metrics[field + '_per_operation'] = spread(values) if values else None
                    values = [m['peak_backend_bytes'] for m in measurements if m['peak_backend_bytes'] is not None]
                    metrics['peak_backend_bytes'] = max(values) if values else None
                    for field in ('backend_bytes_after_collect', 'backend_allocations_after_collect',
                                  'backend_resources_after_collect'):
                        values = [m[field] for m in measurements if m[field] is not None]
                        metrics[field] = spread(values) if values else None
                    metrics['process_peak_rss_bytes'] = spread([m['process_peak_rss_bytes'] for m in measurements])
                    report['variants'][kind] = {'summary': metrics, 'samples': measurements}
                baseline = report['variants']['baseline']['summary']['cpu_ns_per_operation']['median']
                candidate = report['variants']['candidate']['summary']['cpu_ns_per_operation']['median']
                report['candidate_to_baseline_cpu_ratio'] = candidate / baseline
                result['cases'][case] = report
                print(f'{case}: measured; candidate/baseline median CPU {candidate / baseline:.3f}', flush=True)
            result['completed'] = True
    except BaseException as exc:
        failure = exc
        result['failure'] = str(exc)[-12000:]
    finally:
        (args.work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    if failure is not None:
        raise failure
    print('Regex comparison measurements recorded; no performance parity claim', flush=True)


if __name__ == '__main__':
    main()
