<h1 align=center>Genshin Impact NX</h1>

An experimental wrapper/port of the Android release of **Genshin Impact**
(package `com.miHoYo.GenshinImpact`, version `7.0.0`, versionCode 1224). It loads
the original game binary `libyuanshen.so` (Unity 2017.4 / IL2CPP), applies
Android APS2 packed relocations and runs it inside a minimal Android-like
environment natively on the Switch.

The public Mesa 25.0.7 build was verified to reach and remain responsive at the
account-login screen with the 6.7.0 baseline. Client 7.0.0 support is being
revalidated on the `client-1224` branch and must not be treated as hardware-tested
until its binary RVAs and fingerprints are updated. This is not a playable port:
later game startup is still blocked by Genshin Impact's anti-cheat.

It is published as a base for other Android Unity ports.

## Building

The native wrapper requires devkitPro's `devkitA64`, `libnx`, `switch-sdl2`,
`switch-curl`, `switch-zlib`, and `switch-libzstd` packages. Graphics uses a
loaderless Mesa/NVK SDK and must not be linked with the older Switch EGL/GLES
stack.

The original build expects the author's Mesa 26.1.5 SDK at:

```text
vendor/mesa-26.1.5-switch-vulkan-sdk
```

An external self-contained SDK can instead be selected explicitly. It must
contain `include/vulkan/`, including `vulkan_vi.h`, and `lib/libvulkan.a`:

```sh
export DEVKITPRO=/opt/devkitpro
make preflight NVK_SWITCH_ROOT=/opt/nvk-switch
make -j"$(nproc)" NVK_SWITCH_ROOT=/opt/nvk-switch
```

Some external SDK packages omit the `vk_video` headers included by their own
`vulkan_core.h`. Supply the matching Khronos Vulkan-Headers prefix separately
in that case. For a package reporting `VK_HEADER_VERSION 305`:

```sh
make -j"$(nproc)" \
  NVK_SWITCH_ROOT=/opt/nvk-switch \
  VULKAN_HEADERS_ROOT=/opt/Vulkan-Headers-1.4.305
```

The public [jhuz0/nvk-switch](https://github.com/jhuz0/nvk-switch) archive is a
reproducible experimental backend built from Mesa 25.0.7. It is useful for
build and hardware bring-up, but it is not equivalent to the original 26.1.5
SDK and Mesa does not consider NVK conformant on the Switch's Maxwell GPU.

The included GitHub Actions workflow pins that public SDK by SHA-256 and pins
the matching Khronos headers by commit. It builds the wrapper only and rejects
committed APKs, shared libraries, metadata, and other proprietary game inputs.

The build does not need proprietary game files. Runtime files must be supplied
from a legally obtained copy of the exact Android client described above.
See [TESTING.md](TESTING.md) for the SD-card layout and first native hardware
smoke test.
