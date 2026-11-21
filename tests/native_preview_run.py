#!/usr/bin/env python3
"""Run native preview seam fixtures using the existing native compiler config."""
import json
import os
import pathlib
import subprocess
import tempfile
import sys

ROOT = next(parent for parent in pathlib.Path(__file__).resolve().parents if (parent / 'engine-paths.json').is_file())
sys.path.insert(0, str(ROOT / 'tools'))
from engine_paths import WorkspacePaths
LAYOUT = WorkspacePaths.load(ROOT)
CP = LAYOUT.get('bamboo.slicer.root')
CODE = CP / 'src/libslic3r/GCode'
TEST = CP / 'tests'
BUILD = CP / 'build'
CONFIG = LAYOUT.get('bamboo.preview.root') / 'target/native/compiler-configuration.json'


def compiler_flags(flags):
    """Use current source headers and retained generated/dependency inputs.

    The tiny fixture compiles its translation units independently of the core
    library. Its optional old core PCH embeds absolute source paths and is not a
    valid input after relocation; compile these units without that PCH instead.
    Other forced includes and all compiler definitions remain unchanged.
    """
    # CMake may have been generated at a different workspace location.
    sources = {value[2:] for value in flags if value.startswith('-I') and
               value.endswith('/CorvusPrint/src')}
    if len(sources) > 1:
        raise ValueError('Ambiguous recorded native source roots')
    source = next(iter(sources), str(ROOT / 'CorvusPrint' / 'src'))
    current = str(CP / 'src')
    result = []
    items = iter(flags)
    for value in items:
        if value == '-Winvalid-pch':
            continue
        if value == '-include':
            include = next(items)
            if pathlib.Path(include).name == 'cmake_pch.hxx':
                continue
            result.extend((value, include))
            continue
        if value == source or value.startswith(source + '/'):
            value = current + value[len(source):]
        elif value.startswith('-I' + source + '/') or value == '-I' + source:
            value = '-I' + current + value[len('-I' + source):]
        result.append(value)
    return result


def run(command, **kwargs):
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def main():
    temp_root = os.environ.get('TMPDIR', '/dev/shm')
    env = dict(os.environ, TMPDIR=temp_root)
    flags = compiler_flags(json.loads(CONFIG.read_text()))
    with tempfile.TemporaryDirectory(prefix='native-preview-', dir=temp_root) as name:
        tmp = pathlib.Path(name)
        stub = tmp / 'i18n.cpp'
        stub.write_text('#include "I18N.hpp"\nSlic3r::I18N::translate_fn_type Slic3r::I18N::translate_fn = nullptr;\n')
        native = tmp / 'native-fixture'
        run(['/usr/bin/c++', *flags, '-O0', '-ffunction-sections', '-fdata-sections', '-I' + str(CODE),
             TEST/'native_preview.cpp', CODE/'NativePreview.cpp', CODE/'NativePreviewNative.cpp',
             CODE/'NativeProcessorProvenance.cpp', CODE/'NativePreviewPublish.cpp',
             CODE/'NativeProcessorMoveMap.cpp', CODE/'NativeProcessorMoveMapNative.cpp', stub, '-Wl,--gc-sections', '-lcrypto', '-pthread', '-o', native],
            cwd=BUILD, env=env)
        prefix = tmp / 'fixture'
        run([native, prefix], env=env)
        run(['python3', TEST/'native_preview_oracle.py', str(prefix)+'.cpv', str(prefix)+'.json'], env=env)
        run(['python3', TEST/'native_preview_oracle.py', str(prefix)+'.gcode.preview.cpv',
             str(prefix)+'.gcode.preview-map.json', str(prefix)+'.gcode.processor-map.jsonl'], env=env)
        publish = tmp/'publisher-fixture'
        run(['/usr/bin/c++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections', '-I'+str(CODE),
             TEST/'native_preview_publish.cpp', CODE/'NativePreviewPublish.cpp', '-Wl,--gc-sections', '-lcrypto',
             '-o', publish], cwd=ROOT, env=env)
        run([publish, tmp/'publication'], env=env)
        staging = tmp/'staging-fixture'
        run(['/usr/bin/c++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections', '-I'+str(CODE),
             TEST/'native_preview_publish_test.cpp', CODE/'NativePreviewPublish.cpp', '-Wl,--gc-sections', '-lcrypto',
             '-o', staging], cwd=ROOT, env=env)
        run([staging], env=env)
        print('native preview staged publication checks: OK', flush=True)


if __name__ == '__main__':
    main()
