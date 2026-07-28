<div align="center"><img src="./extras/logo.png" width="300"><h1 align="center">Super Mario Bros. Remastered · Nintendo Switch port</h1>
</div>
A wrapper/loader that runs the <b>Android build of Super Mario Bros. Remastered.</b>

(an open-source fan game created by [JoeMama](https://github.com/JHDev2006/Super-Mario-Bros.-Remastered-Public)), GPL-3.0, adapted for Android
in [this repo's sibling source project](https://github.com/mircowuffwuff/super-mario-bros.-remastered-android)) on the Nintendo Switch. It loads the game's
own arm64-v8a
`libgodot_android.so`, provides a minimal Android-like environment (fake JNI, a
bionic→newlib libc shim, and a GLES3/EGL import table backed by mesa), and drives
the engine's `GodotLib` native lifecycle directly. Same technique and wrapper
architecture as the [smwr_nx](https://github.com/delsonazevedo/Super-Mario-World-Remastered-Plus-Switch) port.

### How to install

On your SD card, the app lives in `/switch/smbr_nx/`:

```
/switch/smbr_nx/smbr.nro              <- the port (this repo builds it)
/switch/smbr_nx/libgodot_android.so   <- arm64-v8a engine binary (from the APK)
/switch/smbr_nx/libc++_shared.so      <- arm64-v8a C++ runtime (from the APK)
/switch/smbr_nx/assets/               <- game data (the APK's assets/ folder)
/switch/smbr_nx/save/baserom.nes      <- your legally own Super Mario Bros. (NES) dump.
```

**Place your ROM before first launch**, at exactly `/switch/smbr_nx/save/baserom.nes`.

Launch it through a **game/title override** (hold R on a game and open it) or a
forwarder, so the port runs with full RAM. It will not work in applet/album mode.

Saves and `config.txt` live in `/switch/smbr_nx/save/`.

### How to build

You need devkitA64 (devkitPro) with these packages:

* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-zlib`

```sh
make
```

### Credits
- **JoeMama** and contributors — [Super Mario Bros. Remastered (the game, GPL-3.0)](https://github.com/JHDev2006/Super-Mario-Bros.-Remastered-Public)
- **mircowuffwuff** — [the Android Fork used](https://github.com/mircowuffwuff/super-mario-bros.-remastered-android)
- **fgsfds** and **NaGaa95** — the Switch so-loader groundwork

### Legal

This project has no affiliation with Nintendo. It contains no game code or
assets — supply your own `libgodot_android.so`, `libc++_shared.so` and `assets/`
from the fan game's APK, and your own legally-dumped NES ROM. The game itself is
GPL-3.0 (see the source project's LICENSE); this wrapper's own source is MIT, see
LICENSE.
