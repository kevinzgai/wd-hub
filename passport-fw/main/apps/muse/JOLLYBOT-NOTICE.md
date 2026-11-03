# Jollybot artwork

Copyright (c) Meta Platforms, Inc. and affiliates.

`muse_avatar_frames.c` contains offline-rendered frames from `esp32/avatar/muse_pixel.c` in the official Muse Gadget SDK, commit `4bd647bc805b2e0de49dc894c0680225ce29bd06`.

The official repository explicitly excludes the Jollybot avatar from its Apache-2.0 license. The distributor confirmed separate public-distribution permission on 2026-10-06. This file does not relicense the artwork under Apache-2.0 or MIT, and the frames must not be described as original Passport artwork. The website firmware may distribute the compiled frames under that separate permission; the application-only GitHub source archive keeps the artwork out of its MIT-licensed file set.

Sources:
- https://github.com/facebookincubator/muse-gadget-sdk#license
- https://github.com/facebookincubator/muse-gadget-sdk/blob/main/esp32/tools/muse/AVATAR_RECIPE.md

Thirty 64×64 RGB565 frames are stored as const data in Flash. Runtime playback switches descriptors and uses LVGL's bounded software drawing buffer; it does not load a GIF or allocate an avatar framebuffer. The original procedural renderer is only used offline.
