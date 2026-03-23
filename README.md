# PS Vita Recovery Menu Project

A diagnostic and recovery environment for the PlayStation Vita with plugin repair tools, storage diagnostics, and boot-time recovery via R trigger.

Hi! I'm Jose (@DrinkingSubset), and this is my first major homebrew project for the PS Vita.

I want to be completely transparent from the start:

I'm **not a full-time or professional developer**. For years, I've had this idea in the back of my mind — a proper, reliable custom recovery menu you could boot into by holding R, something that could really help save bricked or misconfigured Vitas. But learning to code from scratch, especially low-level Vita kernel stuff, felt overwhelming and out of reach.

Thanks to modern AI tools (especially Claude and others), I was finally able to turn that long-held dream into something real. **Most of the code in this project was written with significant help from AI**, which guided me through architecture decisions, fixed bugs, explained Vita-specific internals, and helped structure everything safely. I reviewed, tested, and tweaked every part myself on real hardware, but I could not have built this without that assistance.

I'm sharing this openly because I believe in transparency — especially in the homebrew community where trust and safety matter a lot. This project is a learning journey for me, and I'm proud of how far it's come, but it's far from perfect. If you find issues, have suggestions, or want to contribute fixes/improvements, please feel free to open an issue or pull request. I'd love to collaborate and make it even better together.

Thank you for trying it out, and thank you to the entire Vita homebrew community whose tools, plugins, and knowledge made this possible.

— DrinkingSubset ☣️🟩🥕⚡️🚀

March 2026

---

## ⚠️ Disclaimer

**Read this carefully before using PS Vita Recovery Menu.**

This software is provided **as-is**, with no warranty of any kind. Use it entirely at your own risk.

### What this tool can and cannot do

PS Vita Recovery Menu is designed to assist with **software-level** issues on PS Vita systems running custom firmware (HENkaku, h-encore, h-encore2, Ensō). It can help recover from:

- Corrupted or misconfigured `config.txt` files
- Plugin conflicts causing boot loops
- Broken LiveArea databases
- Misconfigured storage mount points
- General CFW misconfigurations

**This tool cannot recover a fully hard-bricked PS Vita.** If your device does not power on, does not display anything on screen, or has suffered damage at the hardware or NAND level, no software tool — including this one — can help. A hard brick at the hardware level requires physical repair or specialized recovery equipment that is beyond the scope of this project.

### Risk of bricking

Certain operations available in this application — including but not limited to modifying system partitions, deleting core system files, resetting the tai configuration, or applying unsafe plugin configurations — **can result in a soft brick or, in worst-case scenarios, a hard brick of your PS Vita**, rendering it permanently unusable.

This applies to **all PS Vita models**, including:
- PS Vita PCH-1000 / PCH-1001 (OLED)
- PS Vita PCH-1100 / PCH-1101 (OLED 3G)
- PS Vita PCH-2000 / PCH-2001 (Slim LCD)
- PlayStation TV (VTE-1000 / CEM-3000)

### Your responsibility

By using this software, you acknowledge and accept that:

1. You are solely responsible for any damage, data loss, or bricking that occurs to your device.
2. The developer of PS Vita Recovery Menu bears no liability for any outcome resulting from the use of this software.
3. You have a basic understanding of PS Vita custom firmware and the risks involved in modifying system files.
4. You have backed up any important data before performing recovery or restoration operations.

### Recommendations before use

- Always back up `ux0:tai/config.txt` and `ur0:tai/config.txt` before making any changes.
- Use the **Backup tai/** function under Restore / Unbrick before proceeding with any repair operation.
- Test changes on one device before applying to others.
- If unsure about an operation, do not proceed.

**This tool is intended for experienced PS Vita CFW users who understand the risks. It is not a magic fix-all solution and should be treated with the same caution as any other system-level utility.**

### No Liability

The creator and developer of PS Vita Recovery Menu (**DrinkingSubset**) is **not responsible** for any damage, data loss, soft brick, hard brick, or any other consequence that occurs as a result of using this software. This includes but is not limited to: accidental deletion of system files, incorrect configuration changes, failed recovery attempts, or any unintended side effects on your device or data.

**You use this software at your own risk. Full stop.**

---

### Screenshot of the Main Recovery Menu

![PS Vita Recovery Menu Screenshot](VitaRecovery%20Screenshots/2026-03-08-161955.png)

*Example of the main menu interface on a PS Vita 1000 (OLED)*

---

## PS Vita Recovery Menu v0.1-pre

A custom recovery environment for the PS Vita (and PSTV) running HENkaku / h-encore / Ensō.
Hold R at power-on to boot directly into the recovery menu.
Provides a complete toolkit for plugin management, system diagnostics, unbricking, storage repair, CPU control, FTP access, and more.

## Features

### Main Menu
* Exit to LiveArea
* Plugins
* Advanced
* System Info
* Restore / Unbrick
* Plugin Fix Mode
* Sony Recovery
* Storage Manager
* File Manager
* Cheat Manager
* Reboot
* Power Off

### Plugin Manager
Toggle any plugin on/off, remove duplicates, clean config.txt, save changes.

### Advanced Tools
* CPU Speed Control — independent ARM / GPU ES4 / BUS / XBR clock domains, persists across reboots
* Registry Hacks
* Reset VSH (restart LiveSpace)
* Suspend / Shut Down / Reboot
* System Write Mode (with full warning dialog)
* Boot Diagnostics
* Boot Recovery Installer

### System Information
Firmware version, model, Ensō status, motherboard series, live clocks, battery health, memory, active tai config path.

### Restore / Unbrick
* Safe Mode Boot
* Reset taiHEN config
* Backup / Restore ux0:tai/
* Rebuild LiveArea Database
* Official Sony recovery options

### Plugin Fix Mode
Safe Mode, View & Toggle, Re-enable All, Reset to Minimal, Backup / Restore config.

### Sony Recovery
Replicas of Sony's safe-mode options (Restart, Rebuild Database, Format Memory Card, Restore System, Update Firmware) with danger warnings.

### Storage Manager (SD2Vita)
Card & config info, mount point switching, StorageMgr install, format tools.

### File Manager
Full partition browser with folder and file operations.

### Cheat Manager
Vita native cheats (.psv) and PSP CWCheat (.db) support.

---

## Installation

Jailbreak required — HENkaku / h-encore / h-encore2 on firmware 3.60–3.74.

1. Download the latest `VitaRecovery.vpk` from the Releases page.
2. Install via VitaShell.
3. Launch the app once from LiveSpace (bubble will appear as Title ID `RECM00001`).
4. Go to **Boot Recovery Installer → Install Boot Recovery**.
   - Copies `boot_recovery.skprx` and `boot_trigger.suprx` to your active tai directory.
   - Inserts both entries into `ur0:tai/config.txt` (or your active config).
   - A backup of your config is automatically created.
5. Reboot holding R to enter the recovery menu.

---

## Build from Source

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build
```

Output: `build/VitaRecovery.vpk`

Requires [VitaSDK](https://vitasdk.org/).

---

## How the R-Trigger Boot System Works

The boot recovery system uses two separate plugins that work together:

```
boot_recovery.skprx  (kernel plugin, loaded under *KERNEL)
boot_trigger.suprx   (user plugin, loaded under *main)
```

**What actually happens at boot:**

```mermaid
flowchart TD
    A[Power On] --> B[ScePsp2BootConfig]
    B --> C[taiHEN loads *KERNEL plugins]
    C --> D[boot_recovery.skprx loads FIRST under *KERNEL]
    D --> E[Spawns a thread that waits for ctrl driver to initialize]
    E --> F{R trigger held?}
    F -->|Yes| G[Writes flag file: ur0:tai/recovery_boot_trigger]
    F -->|No| H[Exits cleanly — zero overhead on normal boot]
    G --> I[boot_recovery.skprx exits]
    H --> I
    I --> J[ScePsp2BootConfig launches shell.self]
    J --> K[SceShell / LiveSpace starts]
    K --> L[taiHEN loads *main plugins into SceShell process]
    L --> M[boot_trigger.suprx loads inside SceShell]
    M --> N{Flag file exists?}
    N -->|No| O[Exits immediately — no overhead]
    N -->|Yes| P[Spawns thread, waits for LiveSpace to be ready]
    P --> Q[Deletes flag file]
    Q --> R[Calls sceAppMgrLaunchAppByUri\npsgm:play?titleid=RECM00001]
    R --> S[Recovery Menu launches]
    O --> T[Normal LiveSpace boot]
```

**Why two plugins?**

`sceAppMgrLaunchAppByUri` requires SceShell to already be running — it cannot be called from kernel space or before the shell initializes. The kernel plugin (`boot_recovery.skprx`) only writes a flag file. The user plugin (`boot_trigger.suprx`) runs inside the SceShell process where AppMgr is available, reads the flag, and launches the recovery app.

---

## Tested Devices

| Device | Firmware | CFW |
|--------|----------|-----|
| PS Vita PCH-1000 (3G) | 3.65 | Ensō |
| PS Vita PCH-1101 | 3.60 | HENkaku |
| PS Vita PCH-1101 | 3.74 | h-encore2 |
| PS Vita PCH-2001 Slim | 3.65 | Ensō + SD2Vita |

---

## Safety Features

- Never touches `vs0:` / `os0:` unless System Write Mode is manually enabled (with full warning dialog).
- Config backed up before every install/uninstall operation.
- Atomic config writes (`.tmp` → rename) prevent corruption on power loss.
- L-trigger at boot triggers safe mode — disables non-essential plugins before menu opens.
- FTP server only activates on demand — zero overhead when not in use.

---

## Known Issues

- PS Vita 2000 model detection shows incorrect model name in System Info — under investigation.
- Boot recovery does not work on h-encore2 (3.74) without Ensō, because the kernel hook requires taiHEN to load at coldboot. Normal bubble launch works fine on all firmware versions.

---

## Future Plans

- Vita 2000 model detection fix
- Live taiHEN module reload
- Per-TitleID CPU clock profiles
- Registry editor
- Full Modoru integration for one-click downgrade/restore
- FTP Server

---

## Credits & Thanks to the Homebrew Scene

This recovery menu stands on the shoulders of giants. The PSP and PS Vita homebrew communities have been collaborative, innovative, and persistent for over two decades. Without their exploits, tools, libraries, and shared knowledge, none of this would exist.

### PSP Scene Pioneers (2005–2010)

- **Dark_AleX** — Creator of OE, SE, and M33 CFW. Often called the father of PSP modding.
- **Team M33** — Continued the M33 CFW line after OE/SE.
- **Total_Noob** — Long-time PSP developer.
- **Fanjita** — Early exploit collaborator.
- **nem** — Created the very first PSP exploit (2005 TIFF on 1.0 firmware).
- **Davee** (Team Typhoon) — ChickHEN for newer PSP models.

### PS Vita Scene (2016–Present)

- **Team Molecule** (yifanlu, Davee, Proxima, xyz, mathieulh, and others) — Created HENkaku, taiHEN, and Ensō. The foundation for all modern Vita homebrew.
- **TheOfficialFloW** — VitaShell, Modoru, Adrenaline, and countless tools.
- **SKGleba** — VitaDeploy, enso_ex, IMCUnlock, and many storage tools.
- **Freakler** — ConsoleID, Fingerprint, and various utilities.
- **xerpi** — ftpvitalib, vita2dlib.
- **Rinnegatamante** — Massive ports, emulators, and game enhancements.
- **cuevavirus** — taiHEN maintenance.
- **devnoname120** — VHBB (Vita Homebrew Browser).
- **LiEnby** — Technical corrections and feedback that improved this project's accuracy.
- Other contributors: 173210, aerosoul, ColdBird, cpasjuste, dots-tb, frangarcj, Hykem, LemonHaze, motoharu, Nkekev, PrincessOfSleeping, qwikrazor87, SilicaAndPina, SocraticBliss, Sorvigolova, St4rk, velocity, and many more.

### Special Thanks
- The entire **r/vitahacks** community for guides, testing, and support.
- **vita.hacks.guide** maintainers — The definitive modern resource.
- **PSDevWiki**, **GameBrew**, and **PSP-Archive** for preserving scene history.
- All plugin authors (StorageMgr, rePatch, NoNpDrm, YAMT, PSVshell, etc.) whose work is used daily.
- Everyone who tested, reported bugs, and gave feedback — especially early testers who helped catch real hardware issues.
