#!/usr/bin/env python3
"""Check Aulay's source inventory, build paths, metadata and ignore rules."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}


def read(name):
    path = ROOT / name
    return path.read_text(encoding='utf-8-sig') if path.exists() else ''


def inventory(tree):
    tags = ('ClCompile', 'ClInclude', 'ResourceCompile', 'Image', 'Manifest', 'None')
    return {(tag, item.get('Include')) for tag in tags
            for item in tree.findall('.//m:' + tag, NS) if item.get('Include')}


def main():
    project = ET.parse(ROOT / 'Aulay.vcxproj')
    items = inventory(project)
    filters = inventory(ET.parse(ROOT / 'Aulay.vcxproj.filters'))
    rc_bytes = (ROOT / 'Aulay.rc').read_bytes()
    rc = rc_bytes.decode('utf-16')
    version = read('Version.h')
    number = re.search(r'^#define AULAY_VERSION_NUMBER ([0-9,]+)$', version, re.M)
    text = re.search(r'^#define AULAY_VERSION_TEXT "([0-9.]+)"$', version, re.M)
    assembly = ET.parse(ROOT / 'Aulay.manifest').getroot().find('{urn:schemas-microsoft-com:asm.v1}assemblyIdentity')
    normal_dirs = [e.text for g in project.findall('m:PropertyGroup', NS)
                   if not g.get('Condition') for e in g.findall('m:IntDir', NS)]
    native_scripts = ('run_windows_tests.cmd', 'run_debug_audio_test.cmd', 'run_debug_monitor_test.cmd')
    native_commands = [line.strip() for name in native_scripts
                       for line in read('tests/' + name).splitlines()
                       if line.strip().lower().startswith('cl ')]
    workflow = read('.github/workflows/build.yaml')
    release = workflow.split('    - name: Create Release\n', 1)[-1]
    arm_sdk = project.find("m:PropertyGroup[@Label='Globals']/m:WindowsTargetPlatformVersion[@Condition=\"'$(Platform)'=='ARM'\"]", NS)
    checks = {
        'ARM32 selects an SDK with ARM32 support': arm_sdk is not None and arm_sdk.text == '10.0.22621.0',
        'CI selects VS2022 for the v143 toolset': 'runs-on: windows-2022' in workflow
            and "vs-version: '[17.0,18.0)'" in workflow
            and '-version "[17.0,18.0)"' in workflow,
        'Release requires all four executable assets': 'fail_on_unmatched_files: true' in release
            and all(path in release for path in ('x64/Release/Aulay64.exe', 'Release/Aulay32.exe',
                                                'ARM64/Release/AulayARM64.exe', 'ARM/Release/AulayARM.exe')),
        'tested x64 artifact is uploaded before other architectures': workflow.index('name: Aulay-x64')
            < workflow.index('name: Build ARM64'),
        'native tests use SDK WinRT headers rather than generated package headers': all('Generated Files' not in read('tests/' + name) for name in native_scripts),
        'native tests use standard C++20 coroutines': len(native_commands) == 5 and all(
            '/std:c++20' in line.split() and not any(option.startswith('/await') for option in line.split())
            for line in native_commands),
        'project references existing source files': all((ROOT / f.replace('\\', '/')).is_file() for _, f in items),
        'project and IDE filters match': items == filters,
        'one application icon asset': {f for tag, f in items if tag == 'Image'} == {'Aulay.ico'},
        'normal intermediate directory is explicit': normal_dirs == ['$(Platform)\\$(Configuration)\\obj\\'],
        'single build variant': 'AulayDiagnostic' not in read('Aulay.vcxproj') and 'Aulay-Debug-x64' not in workflow,
        'assembly identifies Aulay': assembly is not None and assembly.get('name') == 'Aulay',
        'version declarations agree': bool(number and text and number[1].replace(',', '.') == text[1] == assembly.get('version')),
        'resource versions use shared version header': '#include "Version.h"' in rc and all(s in rc for s in ('FILEVERSION AULAY_VERSION_NUMBER', 'PRODUCTVERSION AULAY_VERSION_NUMBER', '"FileVersion", AULAY_VERSION_TEXT', '"ProductVersion", AULAY_VERSION_TEXT')),
        'log version uses shared version header': '#include "Version.h"' in read('Diagnostics.hpp') and 'L"session-start version=" AULAY_VERSION_TEXT' in read('Diagnostics.hpp'),
        'resource encoding has UTF-16LE BOM': rc_bytes.startswith(b'\xff\xfe') and (ROOT / 'translate/generated/translate.rc').read_bytes().startswith(b'\xff\xfe'),
        'CI runs unified portable suite': 'run: python3 tests/run_tests.py' in read('.github/workflows/build.yaml'),
        'CI runs each noninteractive native suite': all('call tests\\' + name in read('.github/workflows/build.yaml') for name in ('run_windows_tests.cmd', 'run_debug_audio_test.cmd', 'run_debug_monitor_test.cmd')),
        'both READMEs show unified test entry point': all('python tests/run_tests.py' in read(name) for name in ('README.md', 'README.zh_CN.md')),
        'MIT notice remains present': 'Copyright (c) 2020 Richard Yu' in read('LICENSE') and 'permission notice shall be included' in read('LICENSE'),
    }
    # Evaluate Git's actual rules, independently of the source tree's index.
    with tempfile.TemporaryDirectory(prefix='aulay-layout-') as tmp:
        subprocess.run(['git', 'init', '-q', tmp], check=True)
        shutil.copyfile(ROOT / '.gitignore', Path(tmp) / '.gitignore')
        visible = [f.replace('\\', '/') for _, f in items] + ['translate/source/messages.pot']
        ignored = ['translate/generated/zh_CN.ymo', 'translate/generated/zh_TW.ymo',
                   'x64/Release/obj/example.obj', 'Aulay.json',
                   'DebugLogs/example.log', 'example:Zone.Identifier']
        def is_ignored(path):
            result = subprocess.run(['git', '-C', tmp, 'check-ignore', '-q', '--no-index', path])
            if result.returncode not in (0, 1):
                raise RuntimeError(f'git check-ignore failed: {path}')
            return result.returncode == 0
        checks['source inputs are not ignored'] = all(not is_ignored(f) for f in visible)
        checks['generated and runtime files are ignored'] = all(is_ignored(f) for f in ignored)
    for label, passed in checks.items():
        if not passed:
            print('FAIL ' + label)
    print(f'PROJECT_LAYOUT {sum(checks.values())}/{len(checks)} passed')
    print('PROJECT_LAYOUT_RESULT=' + ('PASS' if all(checks.values()) else 'FAIL'))
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
