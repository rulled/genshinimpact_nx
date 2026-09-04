# Native hardware smoke test

> **Client 1234 (Genshin 7.0.1) is awaiting hardware verification.** Its RVAs,
> fingerprints, metadata, and signer were statically revalidated against the
> supplied 1224 and 1234 XAPKs. Client 1224 reached the account-login screen on
> real Switch hardware; do not assume 1234 is verified until this smoke test
> completes.

This procedure tests the Horizon/libnx wrapper only. It does not require or
configure a private game server.

## Requirements

- A Switch capable of running Atmosphere homebrew.
- Full application memory through title override, not album applet mode.
- Horizon OS 6.0.0 or newer.
- A FAT32 microSD with enough space for the legally obtained Android client and
  later resource downloads.
- The exact global Android ARM64 client required by `source/config.h`:

```text
Package: com.miHoYo.GenshinImpact
VersionCode: 1234
Version: 7.0.1_47144228_47194594
Unity: 2017.4.30f1
libyuanshen.so SHA-256:
09b2d7593ce48ea30bbdad66983a207672f255dd5c477bc64b27d980dc460eec
```

Do not use an official account for experimental wrapper testing.

## SD layout

Extract the legally obtained APK contents to:

```text
sdmc:/switch/genshinimpact_nx/
```

Place `genshinimpact_nx.nro` in the same directory. Before first launch, the
minimum boot-critical layout is:

```text
switch/genshinimpact_nx/
|-- genshinimpact_nx.nro
|-- lib/arm64-v8a/libyuanshen.so
|-- assets/bin/Data/globalgamemanagers
|-- assets/bin/Data/Managed/Metadata/global-metadata.dat
|-- assets/bin/Data/Managed/Metadata/global-metadata.md5
|-- assets/bin/Data/Managed/Metadata/startup-metadata.dat
|-- assets/bin/Data/boot.config
|-- assets/bin/Data/level0
|-- assets/AssetBundles/data_revision
|-- assets/data_versions_streaming
|-- assets/res_versions_streaming
|-- assets/svc_catalog
`-- certs/cacert.pem
```

Use the `certs/cacert.pem` distributed with this wrapper. Do not replace it
with an untrusted interception certificate.

## Launch

1. Back up `sdmc:/switch/genshinimpact_nx/` before first launch. First boot
   transactionally packs assets and removes redundant loose APK files after
   verification.
2. Hold `R` while launching an installed game to enter hbmenu through title
   override with full application memory.
3. Start `genshinimpact_nx.nro`.
4. Do not enter production credentials. An offline launch is sufficient for
   the first platform smoke test.

Expected early stages include client validation, asset packing, memory tests,
network-service initialization, loading `libyuanshen.so`, Vulkan setup, Unity
constructors, JNI initialization, and the first `nativeRender` call.

## Evidence to retain

After every run, copy these files before launching again:

```text
sdmc:/switch/genshinimpact_nx/fatal.txt
sdmc:/switch/genshinimpact_nx/config.txt
```

Also record:

- every available diagnostic file before the next launch (`run_log.txt`,
  `crash_exit.txt`, `crash_signal.txt`, `stderr.txt`, and `arena_debug.txt`);
- Switch model and RAM/clock configuration;
- Horizon and Atmosphere versions;
- hbmenu/hbloader version;
- whether title override was used;
- NRO SHA-256;
- exact last message visible on screen;
- whether the failure was an error dialog, hang, black screen, or reboot;
- approximate time from launch to failure.

The current public NVK backend is experimental. It has reached the account-login
screen on hardware, but a later Vulkan failure does not by itself prove that the
Android/Unity compatibility layer is broken. Include the complete diagnostic set
and the NRO hash when reporting it.

## Optional harvested device profile

The synthetic Nintendo/Switch identity is rejected by the game's server-side
risk control at game entry. The wrapper can instead report a consistent real
Android device identity harvested over adb from a device that already plays
the supported client:

```powershell
powershell -ExecutionPolicy Bypass -File tools\harvest_device_profile.ps1
```

Copy the generated file to:

```text
sdmc:/config/genshinimpact_nx/device_profile.ini
```

The file is optional; absent keys keep the built-in defaults. Recognized keys:
`model`, `device_name`, `manufacturer`, `brand`, `product`, `device`, `board`,
`hardware`, `platform`, `fingerprint`, `version_release`, `version_sdk`,
`security_patch`, `incremental`, `android_id` (16 hex digits; app-scoped on
modern Android, so the system `settings` value may not match what the game
sees), and `device_fp` (SDK fingerprint session token; usually left unset).
The profile is applied uniformly to system properties, JNI `Build.*` fields,
and the Passport/Combo `x-rpc-device_*` headers.
