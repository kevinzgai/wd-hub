[简体中文](firmware-review-2026-09-15.zh_CN.md) · English

# Passport OS firmware review, 2026-09-15

Base: `manchunx7-bit/passport-os`, `main`, `e5dd58223d62c2be5de6ffcb370b0fcb7b04b11d`.

The changes pass a complete ESP-IDF 5.5.3 build, image merge and protected partition verification. They are published on `feature/retro-firmware-20260915` in [draft PR #1](https://github.com/manchunx7-bit/passport-os/pull/1). Hardware acceptance remains outstanding; main has not been merged.

## Changes

- Centralize terminal colours across boot, home, launcher, settings, both voice applications, radio, Wi-Fi, games and the browser configuration pages. Use dark green surfaces, readable phosphor text, square frames and amber focus. Preserve meaningful artwork and game-piece colours.
- Remove shared animated landscape backgrounds and their retained object pointers. Keep the existing clock-theme API with a stable dark palette.
- Repair the boot title reveal, voice title/status-bar overlap and clock/battery overlap. Bound the shared title/footer widgets. Use the existing broader Chinese font for SSIDs.
- Consume an entire wake gesture; serialize input with app lifecycle changes using a recursive mutex; publish sleep state only after successful display operations.
- Bound Xiaozhi retained text to 1200 bytes at UTF-8 boundaries, for both assignment and streaming appends.
- Fix the missing profile-save token and avatar authorization check, return HTTP 401 correctly, reject oversized fields, bound structure-string reads, and invalidate stale city coordinates.
- Publish Wi-Fi task handles before allowing workers to execute, recover the UI after task-creation failure, and recheck the active page before returning from successful configuration.
- Match the next Tetris piece preview to its actual colour.
- Restore shell executable bits and the missing host-test entry points. Keep existing repository governance checks intact.
- Share C linkage font declarations to fix the radio compilation failure. Use a concrete 8MB flash size for offline merging. Align the verifier with the actual `passport-os.bin` name and committed partition table, including the 4MiB factory partition, while retaining cardid and recovery-area protection. Add a partition-fixture consistency regression.

The upload token is delivered by the device landing page. This is an interface consistency repair, not a replacement for authentication or secure pairing.

## Validation

`bash tools/test-host.sh` passes: animation boundaries, wake-event sequences, UTF-8/emoji/oversized text, 200 Tetris seeds, execution of the actual profile-page save handler, and synthetic firmware protection fixtures. Embedded JavaScript syntax, shell syntax, and diff whitespace checks pass. Materialized base source hashes match the remote tree.

The repository audit still fails on pre-existing missing governance documents, dependency lock, broken links and language peers. These checks were not removed. The original firmware job `104394330260` failed with `./tools/validate.sh: Permission denied`, before compilation.

[Firmware run 34978238572](https://github.com/manchunx7-bit/passport-os/actions/runs/34978238572) passed for code commit `fd73397fc8693b814390ae6095aa87e42cdf22f1`. The application is 3,083,696 / 4,194,304 bytes, leaving approximately 26% of its partition free. Bootloader, partition-table and application comparisons and protected-layout checks all passed; the merged image is 3,149,232 bytes. The run provides artifact `firmware-34978238572`, containing `FoloToy-AI-Passport-full.bin`, retained until 2026-09-22. Subsequent documentation-only commits do not alter these firmware sources.

The hardware-independent regression step also passed in CI. The separate repository audit still fails for the pre-existing missing files and links described above. No actual LVGL screenshot or on-device test has been performed; build success does not establish hardware acceptance.

## Remaining work

Review SPIFFS auto-format-on-mount-failure and avatar replacement recovery; add explicit Wi-Fi session lifetimes for workers that survive stop timeouts; check weather snapshot concurrency and numerical bounds; improve allocation/storage failure feedback; restore accurate upstream project documents and dependency locks.

The complete ESP32-C3 build and protected partition verification have passed. Remaining acceptance includes all-page inspection at 240x320, wake tests for all keys, at least 30 app-switch cycles, interrupted profile uploads, Wi-Fi failure/re-entry tests, long voice replies, ten-minute radio playback, and both games. Keep the PR in draft pending this acceptance. The Chinese companion provides the detailed issue table and acceptance sequence.
