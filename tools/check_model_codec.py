#!/usr/bin/env python3
"""Ensure the persisted hydraulic codec and value rules match the Model.

Checks, against the checked-out AOWIS Model headers:
- the field manifest (docs/model_codec_fields.json) lists exactly the
  persisted structs and fields;
- the codec reads and writes every field in manifest order;
- with --value-rules: every numeric field (and every struct field that
  contains numbers) is checked by a value rule, so a new number can never be
  stored without a decided range;
- with --emit-enums: writes the list of declared enumerators of every
  persisted enum, used by the codec to reject unknown enum values.

The manifest documents the current persisted fields, not a development patch.
A binary codec change must consciously preserve readers for existing data.
"""
import argparse
import json
import pathlib
import re
import sys

NUMERIC_TYPES = {'double', 'int', 'quint64', 'qint64', 'quint32', 'qint32', 'float'}


def strip_comments(data):
    data = re.sub(r'/\*.*?\*/', '', data, flags=re.S)
    return re.sub(r'//[^\n]*', '', data)


def header_files(root):
    files = [root / 'entity.h', root / 'gis.h', root / 'measurement/water_meter.h']
    files.extend(sorted((root / 'hydraulic').glob('*.h')))
    return files


def inspect(root):
    models = {}
    for path in header_files(root):
        data = strip_comments(path.read_text(encoding='utf-8'))
        for found in re.finditer(r'\bstruct\s+(\w+)\s*\{', data):
            depth = 1
            index = found.end()
            while depth:
                if index >= len(data):
                    raise ValueError(f'unterminated {found.group(1)}')
                if data[index] == '{':
                    depth += 1
                if data[index] == '}':
                    depth -= 1
                index += 1
            fields = []
            for line in data[found.end():index-1].splitlines():
                line = line.strip()
                declaration = re.fullmatch(r'(.*?)\s+(\w+)\s*(?:=\s*[^;]+)?;', line)
                if declaration and '(' not in declaration.group(1):
                    fields.append([declaration.group(1), declaration.group(2)])
            models[found.group(1)] = fields
    aliases = {'HydraulicEntityMetadata': 'EntityMetadata'}
    reached = {}

    def visit(type_name):
        type_name = aliases.get(type_name, type_name)
        if type_name in reached:
            return
        if type_name not in models:
            return
        reached[type_name] = models[type_name]
        for field_type, _ in models[type_name]:
            visit(element_type(field_type))
    visit('NetworkHydraulic')
    return dict(sorted(reached.items()))


def element_type(field_type):
    while field_type.startswith('QList<') or field_type.startswith('std::optional<'):
        field_type = field_type[field_type.index('<')+1:-1]
    return field_type


def enums(root):
    """Top-level `enum class` declarations and `using A = B;` aliases."""
    declared = {}
    aliases = {}
    for path in header_files(root):
        data = strip_comments(path.read_text(encoding='utf-8'))
        for found in re.finditer(r'\benum\s+class\s+(\w+)\s*(?::\s*[\w:\s]+)?\{(.*?)\}', data, flags=re.S):
            body = found.group(2)
            names = []
            for item in body.split(','):
                item = item.strip()
                if not item:
                    continue
                if item.startswith('#') or '\n#' in item:
                    names.append(None)  # preprocessor inside the enum
                    continue
                names.append(re.match(r'(\w+)', item).group(1))
            declared[found.group(1)] = names
        for found in re.finditer(r'\busing\s+(\w+)\s*=\s*(\w+)\s*;', data):
            aliases[found.group(1)] = found.group(2)
    return declared, aliases


def check_codec(expected, codec):
    for type_name, fields in expected.items():
        for operation in ('write', 'read'):
            prefix = 'const ' if operation == 'write' else ''
            pattern = (r'void ' + operation + r'Struct\(QDataStream &stream, '
                       + prefix + re.escape(type_name) + r' &value\)\s*\{(.*?)\n\}')
            matches = re.findall(pattern, codec, flags=re.S)
            member_references = (re.findall(r'(?:writeValue|readValue)\(stream, value\.(\w+)\);', matches[0])
                                 if len(matches) == 1 else [])
            expected_names = [name for _, name in fields]
            if member_references != expected_names:
                print(f'{operation} codec for {type_name} does not match the Model fields!',
                      file=sys.stderr)
                print('  Expected:', expected_names, file=sys.stderr)
                print('  Found:', member_references, file=sys.stderr)
                return False
    return True


def check_value_rules(expected, rules, enum_names):
    """Every numeric field, and every field holding a ruled struct, must be
    referenced in that struct's `check(Rules &rules, ...)` function."""
    ruled = set()
    changed = True
    while changed:
        changed = False
        for type_name, fields in expected.items():
            if type_name in ruled:
                continue
            for field_type, _ in fields:
                element = element_type(field_type)
                if element in NUMERIC_TYPES or element in ruled:
                    ruled.add(type_name)
                    changed = True
                    break
    for type_name in sorted(ruled):
        pattern = (r'void check\(Rules &rules, const QString &path, const '
                   + re.escape(type_name) + r' &value\)\s*\{(.*?)\n\}')
        matches = re.findall(pattern, rules, flags=re.S)
        if len(matches) != 1:
            print(f'Missing value rules for {type_name} '
                  f'(void check(Rules &rules, const QString &path, const {type_name} &value)).',
                  file=sys.stderr)
            return False
        referenced = set(re.findall(r'\bvalue\.(\w+)', matches[0]))
        for field_type, name in expected[type_name]:
            element = element_type(field_type)
            if (element in NUMERIC_TYPES or element in ruled) and name not in referenced:
                print(f'Value rules for {type_name} do not check field {name} ({field_type}).',
                      file=sys.stderr)
                print('Decide its valid range in src/model_value_validation.cpp and '
                      'docs/model-value-rules.md.', file=sys.stderr)
                return False
    return True


def emit_enums(expected, root, output):
    declared, aliases = enums(root)
    persisted = set()
    for fields in expected.values():
        for field_type, _ in fields:
            element = element_type(field_type)
            while element in aliases:
                element = aliases[element]
            if element in declared:
                persisted.add(element)
    lines = ['// Generated by tools/check_model_codec.py from the AOWIS Model headers.',
             '// Do not edit. Declared enumerators of every persisted enum.', '']
    for name in sorted(persisted):
        values = declared[name]
        if None in values or not values:
            raise ValueError(f'persisted enum {name} must be a plain list of enumerators')
        items = ', '.join(f'{name}::{value}' for value in values)
        lines.append(f'template<> struct ModelEnumValues<{name}>')
        lines.append('{')
        lines.append(f'    static constexpr {name} values[] = {{{items}}};')
        lines.append('};')
    text = '\n'.join(lines) + '\n'
    output = pathlib.Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding='utf-8') != text:
        output.write_text(text, encoding='utf-8')
    return len(persisted)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('model_root')
    parser.add_argument('manifest')
    parser.add_argument('codec')
    parser.add_argument('--value-rules')
    parser.add_argument('--emit-enums')
    arguments = parser.parse_args()
    root = pathlib.Path(arguments.model_root)
    actual = inspect(root)
    expected = json.loads(pathlib.Path(arguments.manifest).read_text(encoding='utf-8'))
    if actual != expected:
        print('AOWIS Model layout differs from the persisted codec contract.', file=sys.stderr)
        print('Review the fields, add a new codec format version with old-version readers,', file=sys.stderr)
        print('then update the field manifest after reviewing compatibility.', file=sys.stderr)
        for key in sorted(actual.keys() | expected.keys()):
            if actual.get(key) != expected.get(key):
                print(' Changed struct:', key, file=sys.stderr)
                print('   stored:', expected.get(key), file=sys.stderr)
                print('   model: ', actual.get(key), file=sys.stderr)
        return 1
    if not check_codec(expected, pathlib.Path(arguments.codec).read_text(encoding='utf-8')):
        return 1
    if arguments.value_rules:
        declared, _ = enums(root)
        if not check_value_rules(expected, pathlib.Path(arguments.value_rules).read_text(encoding='utf-8'),
                                 declared):
            return 1
    summary = f'AOWIS codec matches {len(actual)} persisted Model structures'
    if arguments.emit_enums:
        try:
            summary += f', {emit_enums(expected, root, arguments.emit_enums)} enums'
        except ValueError as error:
            print(error, file=sys.stderr)
            return 1
    print(summary + '.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
