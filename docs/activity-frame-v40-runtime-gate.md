# v40 Activity frame-import runtime gate

Run the standalone installed-Activity gate from Termux with:

```sh
python3 scripts/test-activity-frame-import-v40-termux.py
```

The script does not install or update an APK. Before it starts any process, it
requires the installed base APK to be byte-identical to the staged
`out/visible-host/bvb-visible-host-debug.apk`, requires both APK identities to
be versionCode 40 with the same signing certificate, and verifies that the
native library embeds `E057_FRAME_TRANSPORT_IMPORTED` and
`E057_FRAME_PRESENTED`. VersionCode 39 is the E042-only predecessor and is
refused explicitly. `--preflight-only` performs just these identity checks and
does not launch the Activity.

The runtime is deliberately independent of Steam and Termux:X11. It starts one
ephemeral Bionic service, proves a wrong 256-bit lifecycle capability receives
`-EACCES`, launches the installed Activity with the real capability using the
previously proven `am start -S` reset and
`bvb_retain_external_renderer=1`, then starts the same-UID
`FrameTransportClient` used by the Tomb Raider launch wiring. The current glibc
global-dispatch smoke client creates the three-image virtual swapchain,
acquires, and presents once. A bounded post-present hold keeps the ring alive
long enough for the Activity to log both E057 completion markers. Cleanup
targets only child PIDs and its private runtime directory; it never signals
Steam or X11. Termux's `am` wrapper has no standalone `force-stop` command, so
the Activity remains visible for human confirmation. The next invocation's
`start -S` resets that exact package and its native state.

Before the Activity gate, the installed bridge payload was updated and checked:
the bridge client is
`c0b3dbf36f45bad941a8579bf37bcc8d5773ac7b4d3c0e10a601b58fc4aee3eb`,
the bridge service is
`0917ef33209b0ea32a337de48646908057854f829387671d0a832ec707371241`,
and private Turnip remains
`8ac6ef78c3c92998aa46c59fd0081edcba82756f5bad561d1b24a57684874a45`.
The `install-pre-0c54e92` rollback directory was verified and the installed
one-shot private-Turnip ICD test passed. The runtime script checks all three
installed artifact hashes again before launch. These are deployment
preconditions, not an Activity runtime pass.

Each invocation gets a unique directory under `out/activity-frame-v40/` with
service, helper, client, Activity logcat, build, cleanup, and `evidence.json`
artifacts. The launch token is neither printed nor written there.

## Acceptance boundary

Success proves that the installed v40 Activity authenticated, imported the
one-time image/control FD bundle, and completed one native copy-or-blit and
`vkQueuePresentKHR`. The E073 global smoke producer does not initialize the
presented swapchain image with a deterministic pattern, so the Activity may
remain black. This gate provides import/present-log proof only: it is not a
screenshot, changing-frame, Tomb Raider, or FPS result.

The later game launcher must mirror the runtime-critical
`bvb_retain_external_renderer=1` Intent extra before releasing its Steam start
gate. That integration should be promoted only after this standalone gate
passes on the tablet.

## Reuse provenance

The required exact `deja` query found no indexed v40 runtime-gate
implementation. This script reuses E010's authenticated lifecycle record and
invalid-token proof, E057's Activity-native import/copy/present markers, E060's
one-time frame socket and persistent futex ring, E073's real private-Turnip
virtual WSI producer proof, and the current `steamclienttermux`
`start-tombraider-bvb-probe.sh` service/helper ordering.
