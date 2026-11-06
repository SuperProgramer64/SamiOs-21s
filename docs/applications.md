# Application compatibility

These are physical observations from the user, not all independently reproduced in QEMU.

| Application | Observed behavior |
|---|---|
| Mail, Photos, TV, Health | Do not open successfully |
| Podcasts | Shows interface, then exits after a few seconds |
| Home | Repeats the notification permission request |
| Fitness, Continuity | Black screen |
| Voice Memos | Black screen followed by a crash |
| Compass | Opens; heading does not change |
| Measure, Magnifier | Crash |

Physical logs linked Voice Memos failures to repeated missing `com.apple.voicememod.datastore.Cloud` requests and an abort reaching `+[RCApplicationModel sharedApplicationModel]`. This is evidence to investigate the service/data-model path, not proof that adding one daemon fixes the app.

A separate QEMU experiment added selected on-demand services (`voicememod`, `tccd`, `healthd`, `healthappd`, `maild`) and app exception logging. These experimental service additions are **not installed in the physical v11 candidate**. Voice Memos launched but the captured interface remained black. Mail-related CloudKit initialization also reported a missing `com.apple.cloudd`; this does not by itself identify the primary opening failure.

QEMU produced a concrete Magnifier exception: `-[AVSpeechSynthesizer setIsInternalSynth:]: unrecognized selector sent to instance`. The app/runtime version and method availability need investigation before a compatibility fix. It is not established as a camera failure, and no blanket missing-method bypass is installed.

Compass requires heading/motion integration, including a magnetometer; a gyroscope alone is insufficient. Camera, motion and several background services remain unavailable. App diagnostics and experimental source changes must be distinguished from fixes verified on the phone.
