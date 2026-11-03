# Passport OS

<!-- impeccable:product-schema 1 -->

## Platform

Embedded ESP32-C3 firmware with LVGL on a 240×320 screen. This is not a web, iOS or Android application. The browser artifact is a separate interaction preview.

## Users and purpose

The owner and people using the same AI Passport hardware use its physical buttons, display, microphone and speaker for portable voice input, assistants, radio and small utilities/games. The owner requested detailed setup documentation for other users.

## Operating context

The device has three physical buttons. Existing Wi-Fi and personal information are configured through a phone web page. Muse pairs with the owner's phone account; this owner's current network uses a computer HTTP proxy. Hold UP to record, release to send, OK pages a received reply, long OK returns. Muse and desktop voice-input are different apps.

## Capabilities and constraints

- ESP32-C3, 8 MB Flash, no PSRAM; preserve memory for Wi-Fi, encrypted connections and recording.
- App switching must release foreground resources. No black "returning" screens.
- Two consecutive Muse hardware reply displays were confirmed by the owner before this visual update. Long-term stability is not established by that short test.
- No supported Muse quota interface was found in the reviewed public SDK documentation. User explicitly requested removing quota from the UI.
- Default system theme is classic green; keep the alternative travel theme and existing button semantics.

## Muse visual commitment

On 2026-10-05 the user requested the displayed official Jollybot character as the Muse app's main visual, complete state motion/transitions, and explicit attention to memory stability. This is a scoped Muse app update. Official character assets have separate rights from SDK code; no public distribution is implied by the local hardware request.

## Evidence

Current firmware source, `DESIGN.md`, `docs/muse.md`, `docs/muse-visual.md`, and local diagnostics in the parent workspace document implementation and validation. Browser simulations, host LVGL snapshots and physical-device tests must be labelled separately.
