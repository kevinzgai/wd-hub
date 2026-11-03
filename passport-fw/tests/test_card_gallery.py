"""Run actual bounded gallery storage with ASan/UBSan and real JPEG fixture."""
import subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='passport-cards-') as tmp:
    target=Path(tmp)/'cards'
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
        '-Wno-unused-variable','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-DSPIFFS_BASE="'+(Path(tmp)/'storage').as_posix()+'"','-Itests/home_mocks','-Itests/radio_mocks','-Imain',
        'tests/test_card_gallery.c','main/apps/home/home_profile.c','-lm','-o',str(target)],cwd=ROOT,check=True)
    subprocess.run([str(target),(ROOT/'tests/fixtures/card-240x320.jpg').as_posix()],check=True)
