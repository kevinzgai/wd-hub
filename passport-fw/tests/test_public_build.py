"""Exercise the actual component CMake with hostile local proxy defaults."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='passport-public-') as directory:
    source = Path(directory)
    (source / 'sdk').mkdir()
    shutil.copyfile(ROOT / 'components/passport_muse/sdk/muse_proxy_defaults.h.in',
                    source / 'sdk/muse_proxy_defaults.h.in')
    (source / 'dummy.c').write_text('int proxy_probe;\n', encoding='utf-8')
    component = (ROOT / 'components/passport_muse').as_posix()
    (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(public_defaults_probe C CXX)
set(PROBE_DIR "${{CMAKE_CURRENT_LIST_DIR}}")
function(idf_component_register)
    add_library(proxy_probe STATIC "${{PROBE_DIR}}/dummy.c")
    set(COMPONENT_LIB proxy_probe PARENT_SCOPE)
endfunction()
set(CMAKE_CURRENT_SOURCE_DIR "{component}")
include("{component}/CMakeLists.txt")
''', encoding='utf-8')
    environment = dict(os.environ, MUSE_HTTP_PROXY_HOST='192.0.2.77', MUSE_HTTP_PROXY_PORT='1234')
    cases = [('local', [], '192.0.2.77', 1234),
             ('public', ['-DPASSPORT_PUBLIC_RELEASE=ON'], '', 0),
             ('override', ['-DPASSPORT_PUBLIC_RELEASE=ON', '-DMUSE_DEFAULT_PROXY_HOST=192.0.2.88',
                           '-DMUSE_DEFAULT_PROXY_PORT=9999'], '', 0)]
    for name, flags, host, port in cases:
        build = source / name
        result = subprocess.run(['cmake', '-S', str(source), '-B', str(build), *flags],
                                env=environment, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        defaults = (build / 'muse_proxy_defaults.h').read_text(encoding='utf-8')
        assert f'#define MUSE_DEFAULT_PROXY_HOST "{host}"' in defaults
        assert f'#define MUSE_DEFAULT_PROXY_PORT {port}\n' in defaults
print('Public build: real component CMake clears environment and explicit proxy overrides: PASS')
