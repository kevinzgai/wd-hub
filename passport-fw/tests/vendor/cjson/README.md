# Host-test JSON dependency

These are unmodified `cJSON.c`, `cJSON.h`, and the MIT `LICENSE` copied from
the cJSON submodule of the ESP-IDF 5.5.3 toolchain used to build this candidate.
Version: 1.7.19. Upstream: https://github.com/DaveGamble/cJSON.
Submodule commit: `c859b25da02955fef659d658b8f324b5cde87be3`.

This test-only snapshot lets GitHub's host regressions run without installing
ESP-IDF. Firmware continues using ESP-IDF's JSON component. No new runtime JSON
dependency or copy is linked into the device.
