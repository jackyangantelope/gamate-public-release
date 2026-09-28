# picoGamate public candidate

Gamate emulator port for the Adafruit Fruit Jam (RP2350B). This is a source and build candidate, not a hardware-validated release.

The firmware does not contain a Gamate BIOS or games. Supply your own lawful 4096-byte BIOS at `/BIOS/GAMATE/gamate_bios.bin` on the SD card and game ROMs in `/roms/GAMATE` (`.bin` or `.rom`, 16-512 KiB). The program refuses to start if the BIOS is absent or has a different size.

The build targets the resident-loader application partition beginning at flash address `0x10080000`; do not install this UF2 as a standalone full-flash image. The default input comes from Fruit Jam controls and USB host. HDMI output is 640×480 with stereo 44.1 kHz audio.

Build on Windows with Pico SDK 2.3+, Pico-PIO-USB, an ARM toolchain, Ninja, and CMake:

```powershell
./scripts/build-release.ps1 -PicoSdkPath C:\path\to\pico-sdk -PicoPioUsbPath C:\path\to\Pico-PIO-USB -ToolchainPath C:\path\to\toolchain -NinjaPath C:\path\to\ninja.exe
```

Optional `-PioasmDir`, `-PicotoolDir`, and `-TinyUsbPath` parameters support prebuilt host tools and an external TinyUSB checkout.

The legacy development firmware was built for a different board and used a different CPU core. Its test result does not transfer to this candidate. Hardware verification must cover boot, controls, video, audio, and representative games before release.

## License and credit

The original port code is BSD-3-Clause under [LICENSE](LICENSE). The combined firmware also includes GPL-3.0 code, so redistributors must meet that license's source-distribution obligations. Component-level credits and remaining provenance questions are in [THIRD_PARTY.md](THIRD_PARTY.md).

Sources: [floooh/chips](https://github.com/floooh/chips), [PicoPlus-devel/pico_shared](https://github.com/PicoPlus-devel/pico_shared), and [xrip/gamate](https://github.com/xrip/gamate).
