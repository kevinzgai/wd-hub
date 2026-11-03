# Muse artwork variants

`muse_avatar_public_frames.c` is an original geometric robot, MIT licensed,
copied from this project's public Muse port. Its generator is
[tools/generate_avatar.py](https://github.com/manchunx7-bit/ai-passport-muse/blob/main/tools/generate_avatar.py).

`muse_avatar_public_frames.c` remains available as the MIT-licensed geometric
fallback. `muse_avatar_frames.c` contains the official Jollybot frames and is
covered by the separate notice in `JOLLYBOT-NOTICE.md`.

The website release build sets `PASSPORT_PUBLIC_RELEASE=ON` to force empty
network defaults, and independently sets `PASSPORT_MUSE_USE_JOLLYBOT=ON` to
select Jollybot. Keeping these switches separate prevents artwork selection
from reintroducing a maintainer's proxy or other personal configuration.

Both assets use the same 30-frame, 64 by 64 RGB565 interface; playback
does not add a runtime image buffer.
