# PS Vita Recovery Menu Project

A diagnostic and recovery environment for the PlayStation Vita with plugin repair tools, storage diagnostics, and planned boot-time recovery via R trigger.

Hi! I'm Jose (@DrinkingSubset), and this is my first major homebrew project for the PS Vita.

I want to be completely transparent from the start:

I'm **not a full-time or professional developer**. For years, I've had this idea in the back of my mind — a proper, reliable custom recovery menu you could boot into by holding R, something that could really help save bricked or misconfigured Vitas. But learning to code from scratch, especially low-level Vita kernel stuff, felt overwhelming and out of reach.

Thanks to modern AI tools (especially Grok and others), I was finally able to turn that long-held dream into something real. **Most of the code in this project was written with significant help from AI**, which guided me through architecture decisions, fixed bugs, explained Vita-specific internals, and helped structure everything safely. I reviewed, tested, and tweaked every part myself on real hardware, but I could not have built this without that assistance.

I'm sharing this openly because I believe in transparency — especially in the homebrew community where trust and safety matter a lot. This project is a learning journey for me, and I'm proud of how far it's come, but it's far from perfect. If you find issues, have suggestions, or want to contribute fixes/improvements, please feel free to open an issue or pull request. I'd love to collaborate and make it even better together.

Thank you for trying it out, and thank you to the entire Vita homebrew community whose tools, plugins, and knowledge made this possible.

— DrinkingSubset ☣️🟩🥕⚡️🚀

March 2026

## PS Vita Recovery Menu v1.0

A powerful custom recovery environment for the PS Vita (and PSTV) running HENkaku Ensō.
Hold R trigger at power-on to boot directly into the recovery menu — before the LiveArea (SceShell) loads.
This project gives you a complete toolkit for plugin management, system diagnostics, unbricking, storage repair, and more — all from a single, safe boot-time entry point.

**Current status note:** The R-trigger boot redirect is fully coded and installs successfully, but it is not working yet on tested hardware (the kernel hook does not fire reliably). You can still launch the recovery menu normally from the bubble or via manual taiHEN entry. We are actively debugging the timing issue. The rest of the menu works perfectly.

## Features

### Main Menu
- Exit to LiveArea
- Plugins
- Advanced
- System Info
- Restore / Unbrick
- Plugin Fix Mode
- Sony Recovery
- Storage Manager
- File Manager
- Cheat Manager
- Reboot
- Power Off

### Plugins Manager
Toggle any plugin, remove duplicates, clean config.txt, re-enable missing files, save changes.

### Advanced Tools
- CPU Speed presets (not working yet)
- Registry Hacks
- Reset VSH (restart LiveArea)
- Suspend / Shut Down / Reboot
- System Write Mode (with full warning dialog — enables os0/vs0 writes)
- Boot Diagnostics (detailed health check)
- Boot Recovery Installer (one-click install/uninstall of the R-trigger plugin)

### System Information
Firmware, model, Enso status, motherboard, clocks, battery health, memory, active tai config path, mount points.

### Restore / Unbrick
- Safe Mode Boot
- Reset taiHEN config
- Backup / Restore ux0:tai/
- Rebuild LiveArea Database
- Official Sony recovery options

### Plugin Fix Mode
Safe Mode (disable all non-essential plugins), View & Toggle, Re-enable All, Reset to Minimal, Backup / Restore config.

### Sony Recovery
Exact replicas of Sony’s safe-mode options (Restart, Rebuild Database, Format Memory Card, Restore System, Update Firmware) with clear danger warnings.

### Storage Manager (SD2Vita)
- Card & Config Info
- Switch mount points (ux0 / uma0 / grw0)
- Install StorageMgr plugin
- Copy ux0 ↔ SD2Vita (both directions)
- Format SD card / Erase SD2Vita data (with red danger labels)

### File Manager
Full partition browser (ux0, ur0, vs0, os0, etc.) with create folder and operations support.

### Cheat Manager
- Vita Native Cheats (.psv) via VitaCheat
- PSP CWCheat (.db) support
- Changes saved to disk and applied on next game launch.

## Installation

Jailbreak required — HENkaku Ensō on firmware 3.60–3.74.
Download the latest PSVita-Recovery-Menu.vpk.
Install the VPK using VitaShell or molecularShell.
Launch the app once (bubble will appear as Title ID RECM00001).
Go to Boot Recovery Installer → Install Boot Recovery.
This copies boot_recovery.skprx to ur0:recovery/ and adds it under *KERNEL in your active tai config (ur0 or ux0).
A backup of your config.txt is automatically created.

Reboot.

To launch the menu normally (while R-trigger is being fixed):
Just open the recovery bubble from LiveArea.

## Build & Development

This project is built using the open-source **VitaSDK** toolchain.

Official site: https://vitasdk.org/
Documentation: https://docs.vitasdk.org/

Huge thanks to the VitaSDK team for making Vita homebrew development accessible and powerful. Without VitaSDK, recompiling and extending this project wouldn't be possible.

To build from source:
1. Install VitaSDK (follow https://vitasdk.org/getting-started/).
2. Set the `VITASDK` environment variable.
3. Run `cmake .` then `make` in the project root (uses CMakeLists.txt).
4. Output: .self and .vpk files ready for install.

Feel free to fork, tweak, and PR!

## How the R-Trigger Boot Plugin Works (Conceptual Flow)

```mermaid
flowchart TD
    A[Power On / Cold Boot] --> B[Enso Bootloader]
    B --> C[taiHEN loads kernel plugins]
    C --> D[boot_recovery.skprx module_start runs]

    subgraph "Early Boot Phase - Critical Timing Window"
    D --> E[Debounced polling: ksceCtrlPeekBufferPositive<br>Multiple reads + 5 ms delays<br>Check for SCE_CTRL_RTRIGGER / LTRIGGER]
    end

    E -->|R/L held → success| F[Install one-shot hook on<br>SceAppMgr!sceAppMgrLaunchAppByUri<br>NID 0xFC4CFC30]
    E -->|No trigger / garbage / not ready → fail| G[Exit immediately<br>No hook installed<br>Zero overhead on normal boot]

    subgraph "Potential Failure: Controller Init Delay"
    H[Controller driver not fully ready yet<br>→ Returns garbage / all zeros / invalid data<br>Common on 3.60 Enso cold boots or certain models]
    end

    E -.->|Misses trigger due to delay| H
    H --> G

    F --> I[Later: SceShell calls sceAppMgrLaunchAppByUri<br>to start LiveArea]
    I --> J[Hook fires → one-shot g_triggered flag]
    J --> K[Write flag files in ur0:tai/]
    K --> L[Redirect URI to psgm:play?titleid=RECM00001]
    L --> M[TAI_NEXT → normal launch path but to recovery app]
    M --> N[Recovery menu loads instead of LiveArea]

    G --> O[Continue normal boot to LiveArea]
