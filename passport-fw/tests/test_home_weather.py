"""Exercise actual profile/weather C code with deterministic NVS and HTTP."""
import os
import subprocess
import tempfile
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CJSON=ROOT/'tests/vendor/cjson'
with tempfile.TemporaryDirectory(prefix='passport-weather-') as temporary:
    executable=Path(temporary)/'weather'
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-Itests/home_mocks','-Itests/radio_mocks','-Imain','-I'+str(CJSON),
                    'tests/test_home_weather.c','main/apps/home/home_profile.c',
                    'main/apps/home/home_weather.c',str(CJSON/'cJSON.c'),'-lm','-o',str(executable)],cwd=ROOT,check=True)
    subprocess.run([str(executable)]+sys.argv[1:],check=True)
