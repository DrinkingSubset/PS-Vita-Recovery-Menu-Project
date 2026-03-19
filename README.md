# PS Vita Recovery Menu

A unified PS Vita homebrew recovery environment for HENkaku / h-encore / Ensō devices.

## Features
- R-trigger boot recovery (hold R at power-on to launch recovery menu)
- Plugin manager with enable/disable toggle
- CPU / bus clock control
- System information screen
- Restore / unbrick tools
- Official Sony recovery functions
- SD2Vita management
- Cheat manager
- Boot Recovery Installer (auto-installs kernel + user plugins)

## Supported Devices & Firmware
- PS Vita 1000 (PCH-10xx) — 3.60, 3.65
- PS Vita 1100 (PCH-11xx) — 3.60, 3.65, 3.74
- PS Vita 2000 (PCH-20xx) — 3.60, 3.65
- PS TV — 3.60, 3.65

## Building
```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build
```
Output: `build/VitaRecovery.vpk`

## Project Structure
```
VitaRecovery/
├── source/              Main app source files
├── boot_plugin/         Kernel plugin (button detection at boot)
├── boot_trigger_user/   User plugin (launches recovery from user space)
├── sce_sys/             VPK assets (icon0.png)
└── CMakeLists.txt       Unified build — compiles all three targets
```

## Boot Recovery
The two-plugin system works as follows:
1. `boot_recovery.skprx` — kernel plugin loaded by taiHEN, polls R/L trigger at boot and writes a flag file
2. `boot_trigger.suprx` — user plugin loaded by taiHEN under `*main`, waits for LiveArea to be ready then launches the recovery app

Both are installed automatically via **Advanced → Boot Recovery Installer** inside the app.

## License
MIT
