#!/usr/bin/env python3
"""Tests of tools/check_model_codec.py: the build must fail when a numeric
Model field has no value rule, and the generated enum table must list every
declared enumerator of every persisted enum.

Usage: check_model_codec_tests.py SOURCE_DIR
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def run(source, *arguments, model=None, rules=None):
    command = [sys.executable, str(source / 'tools/check_model_codec.py'),
               str(model or source / 'external/AOWIS-SERVER-MODEL/include/aowis/model'),
               str(source / 'docs/model_codec_fields.json'),
               str(source / 'src/model_codec.cpp'),
               '--value-rules', str(rules or source / 'src/model_value_validation.cpp')]
    command.extend(arguments)
    return subprocess.run(command, capture_output=True, text=True)


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    source = pathlib.Path(sys.argv[1]).resolve()
    rules_text = (source / 'src/model_value_validation.cpp').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        output = scratch / 'enums.inc'

        result = run(source, '--emit-enums', str(output))
        expect(result.returncode == 0, 'unchanged sources rejected: ' + result.stderr)
        generated = output.read_text(encoding='utf-8')
        count = len(re.findall(r'struct ModelEnumValues<', generated))
        expect(count == 39, f'expected 39 persisted enums, got {count}')
        expect('HydraulicLinkValveType::PCV' in generated, 'valve enumerators missing')
        expect('HydraulicNodeElevationInputType::TerrainElevationAndOffset' in generated,
               'aliased enum missing')
        expect('HydraulicSimulationTimestepEventType' not in generated, 'non-persisted enum emitted')
        before = output.stat().st_mtime_ns
        expect(run(source, '--emit-enums', str(output)).returncode == 0, 'second run failed')
        expect(output.stat().st_mtime_ns == before, 'unchanged table rewritten (forces rebuilds)')

        # A numeric field without a rule fails the build.
        missing_field = scratch / 'missing_field.cpp'
        missing_field.write_text(rules_text.replace(
            '    rules.atLeast(field(path, "leak_area_mm2_per_100m"), value.leak_area_mm2_per_100m, 0.0);\n',
            '', 1), encoding='utf-8')
        result = run(source, rules=missing_field)
        expect(result.returncode == 1 and 'leak_area_mm2_per_100m' in result.stderr,
               'unchecked numeric field accepted')

        # A struct field holding numbers without a rule fails the build.
        missing_struct = scratch / 'missing_struct.cpp'
        missing_struct.write_text(rules_text.replace(
            '    check(rules, field(path, "emitter"), value.emitter);\n', '', 1), encoding='utf-8')
        result = run(source, rules=missing_struct)
        expect(result.returncode == 1 and 'emitter' in result.stderr, 'unchecked struct field accepted')

        # A struct without any rule function fails the build.
        missing_function = scratch / 'missing_function.cpp'
        missing_function.write_text(rules_text.replace(
            'const HydraulicNodeJunctionEmitter &value)', 'const HydraulicNodeJunctionEmitterX &value)', 1),
            encoding='utf-8')
        result = run(source, rules=missing_function)
        expect(result.returncode == 1 and 'HydraulicNodeJunctionEmitter' in result.stderr,
               'missing rule function accepted')

        # A persisted enum with conditional enumerators cannot be listed.
        model = scratch / 'model'
        shutil.copytree(source / 'external/AOWIS-SERVER-MODEL/include/aowis/model', model)
        types = model / 'hydraulic/hydraulic_types.h'
        types.write_text(types.read_text(encoding='utf-8').replace(
            '    TCV,\n', '#ifdef AOWIS_EXTRA\n    TCV,\n#endif\n', 1), encoding='utf-8')
        result = run(source, '--emit-enums', str(scratch / 'other.inc'), model=model)
        expect(result.returncode == 1 and 'HydraulicLinkValveType' in result.stderr,
               'conditional enumerators accepted')

        # A new Model field fails the manifest check.
        links = model / 'hydraulic/hydraulic_links.h'
        shutil.copy(source / 'external/AOWIS-SERVER-MODEL/include/aowis/model/hydraulic/hydraulic_types.h', types)
        links.write_text(links.read_text(encoding='utf-8').replace(
            '    double minor_loss_coefficient = 0.0;\n',
            '    double minor_loss_coefficient = 0.0;\n    double new_number = 0.0;\n', 1), encoding='utf-8')
        result = run(source, model=model)
        expect(result.returncode == 1 and 'Changed struct' in result.stderr, 'new Model field accepted')
    print('check_model_codec.py tests passed')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print('check_model_codec.py test failed:', error, file=sys.stderr)
        raise SystemExit(1)
