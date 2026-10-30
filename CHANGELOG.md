# Changelog — PS Vita Recovery Menu

By DrinkingSubset. Newest version first. Copy a version's block straight into
the GitHub release notes.

## v1.2.0
> **After installing, open Advanced > Boot Recovery > Install / Update Boot Recovery, then reboot.** The kernel boot plugin changed (it applies layout restores). The app now reminds you of this when it starts. "Enable Unsafe Homebrew" must stay ticked.

- **New: Home Screen Layout** (main menu). Back up and restore your home screen: pages, folders (with their names and icons) and bubble order.
  - **Back Up Layout** copies `ur0:shell/db/app.db` to `ux0:data/vita_recovery/layout/app.db` and checks the copy with SQLite before it replaces the old backup. The backup before it is kept as `app.db.prev` ("Older" on screen). If nothing changed since the last backup, nothing is rotated.
  - **Restore Latest / Older Backup** builds the restored database on a copy (the live `app.db` is never edited), shows what will happen (apps put back, apps no longer installed, new apps added at the end) and restarts the Vita. Early in the next boot the boot plugin swaps it in and keeps the replaced database as `ur0:shell/db/app.db.prelayout`. The result is shown on the main menu.
  - Each page's look (wave colour / background) comes back from the backup too.
  - Apps installed after the backup are added after the last bubble; apps no longer installed are skipped; a folder whose apps are all gone is left out.
- **Rebuild Database now backs up your layout first**, so you can put folders and bubble order back afterwards with Home Screen Layout > Restore Latest Backup.
- **New: boot plugin update reminder.** When the app starts (or you come back to the main menu from another screen) and the boot plugins on the Vita are older than the ones inside the app (or only partly installed), a pop-up explains it and offers to open Boot Recovery right away. Choose "Later" and the main menu shows a reminder line until they are updated. Nothing is shown if Boot Recovery was never installed.
- Uses the Vita's own SQLite module (no extra library). Works with Ensō only (like Rebuild Database).
- Tested on a PS Vita 2000 and two PCH-1101 3G (3.60 / 3.65 Ensō, all with SD2Vita): Rebuild Database, then Restore Latest Backup brought back all folders, bubble order and the wave colour. The update reminder was tested by removing a plugin file (pop-up on the main menu, then repaired with Install / Update).

## v1.1.9
> **Requires HENkaku Settings > "Enable Unsafe Homebrew" to be ticked** (the same setting VitaShell needs). Without it the app will not launch. No boot plugin changes — just install the VPK.

- **The app is now built as "unsafe" homebrew.** It can read all system drives, which is needed for correct System Info (and for layout backup/restore planned for v1.2.0).
- **System Info – real firmware:** now asks the system's VSH bridge, which returns the real version even with HENkaku's Version Spoofing on (tested: 3.65 Enso slim spoofed to 3.74 now shows "3.65 (system reports 3.74)"). The firmware tier is now correct too, so screens that depend on it (like the Rebuild Database Enso note) no longer get a spoofed value. The debug line shows every source: `raw` (partition), `mod` (os0 module header), `vsh`, `sys`.
- **System Info – model and motherboard:** taken from the hardware info chip when available (USS = PS Vita 2000, IRS = 1000, IRT = 1100 3G, DOL = PS TV). This fixes the Slim 2000 showing as "1000 (OLED)" (tested: USS-1002), and 3G units now show as "PS Vita 1100 (3G OLED)" with an IRT-002 (3G) board (tested on two PCH-1101).
- **New "Drive access" section in System Info:** shows which drives the app can see and whether they are RW or RO, plus whether `app.db` is readable. The Detection debug section also shows the raw hardware info bytes.
- **Write protection for system drives:** File Manager and the FTP server can read every drive but can only **write** to user storage (`ux0: ur0: uma0: imc0: xmc0: grw0:`). `os0: vs0: sa0: pd0: tm0: ud0: vd0:` and raw devices are always read-only (upload, delete, rename, new folder are refused). Before, File Manager allowed writes to `vd0:`, and FTP had no checks.
- FTP lists the system drives too (read-only), and a small handle leak when listing drives is fixed.
- File Manager: on read-only drives, Delete and Rename are greyed out and labelled "(read-only drive)".

## v1.1.8
- **New: Plugin Fix Mode > Undo Safe Mode.** Puts back your exact config from before Safe Mode ran, including plugins you had deliberately turned off. Safe Mode now keeps that copy every time it runs: in Plugin Fix, at L-trigger boot (`<config>.before_safemode`), and in Restore / Unbrick (`tai_backup/<drive>_config_before_safemode.txt`). Undo uses the newest one.
- Running Safe Mode a second time no longer overwrites that copy with the already-disabled config. Plugin Fix tells you when Safe Mode is already active.
- "Re-enable All" now says what it really does: it turns on **all** plugins, including ones you had turned off.
- Fix: the **Plugins** list now re-reads `config.txt` every time you open it. Before, it was read once at app start, so after Plugin Fix or Restore / Unbrick changed the config, it still showed the old ON/OFF states until the app restarted.
- Plugin Fix header now reads "N plugins in config: X ON, Y OFF" instead of "N plugins loaded", which sounded like N were running.
- Plugin Fix backup / restore / undo now handle configs up to 128 KB (was 16 KB).
- `restore.log` no longer grows forever: over 32 KB it's moved to `restore.log.old` and a new log starts.

## v1.1.7
Safety release based on an outside code review of v1.1.6.

> **After installing, open Advanced > Boot Recovery > Install / Update Boot Recovery, then reboot.** Both boot plugins changed.

- **Boot entry (hold R) is more reliable and can't get stuck.** `boot_trigger.suprx` now launches the menu when the kernel plugin saw R/L held during early boot, not only if R is still held when the home screen loads. Every wait now has a time limit: 60 s for LiveArea, 15 s of launch retries. Before, both loops ran forever, a likely cause of the "custom boot splash + R" boot loop.
- **Boot loop breaker:** after two boots where recovery was requested but the app never started, the plugin stops auto-launching and the Vita boots normally. Flags left over from an earlier boot are cleared at the start of each boot.
- **Boot Recovery installer can update in place.** "Install Boot Recovery" is now "Install / Update Boot Recovery". When the plugins are already installed, it copies this version's plugin files over the old ones (config untouched) instead of saying "uninstall first". The status line shows "UPDATE AVAILABLE" when the installed plugins differ from the app's.
- **Rebuild Database keeps the old database:** the boot plugin renames `app.db` to `app.db.bak` instead of deleting it.
- **Restore / Unbrick is safer:**
  - Safe Mode Boot and Reset taiHEN Config back up each config first (to `ux0:data/vita_recovery/tai_backup/`) and refuse to change anything if the backup fails.
  - Both now **keep HENkaku, storagemgr / gamesd (SD2Vita) and the boot recovery plugins enabled**. Before, they disabled or dropped them, so on SD2Vita ux0: (and this app) could disappear.
  - Safe Mode keeps blank lines and line endings. Reset no longer silently overwrites an existing `ur0:tai/config.txt` fallback.
  - "Rebuild LiveArea Database" now runs the same boot plugin flow as Sony Recovery. The old direct delete never worked and reported "already gone" as success.
- **Safe config writes everywhere:** Restore / Unbrick, Plugin Fix (backup/restore/reset) and the Recovery Installer now write to a temp file, read it back, and swap it in with the old file kept as `.old` until the swap succeeds. A power cut can no longer leave the config missing.
- Plugin Fix "Reset to Minimal" keeps storagemgr / gamesd / boot recovery plugin lines.
- **Sony Recovery > Restore PS Vita System is now an honest guide.** It explains how to factory reset with Sony's Safe Mode (R + PS + Power) instead of running a fake "restore". The old code only started and stopped a system module and rebooted. The Safe Mode hint in the footer now says R+PS+Power.
- **"System Write Mode" removed from Advanced:** the toggle did nothing (File Manager always keeps os0:/vs0: read-only).
- Repo cleanup: removed old unused copies `source/boot_recovery.c`, `source/boot_trigger_user.c`, `source/CMakeLists.txt`, and the stale prebuilt `boot_recovery.skprx` / `boot_trigger.suprx` in the project root (the VPK always uses freshly built plugins). `.gitignore` covers build folders.
- README rewritten for the current version: features, install/update steps, the real boot flow, safety features, known issues.

## v1.1.6
- **Rebuild Database now works**, through the boot plugin. The app is a safe homebrew and can't see `ur0:shell`, so it now writes a request file (`ur0:tai/rebuild_db_trigger`). At the next boot, before the home screen starts, `boot_recovery.skprx` deletes `ur0:shell/db/app.db` and the system rebuilds it. The result appears as a notice on the main menu.
  - Requires the **updated boot plugin**: reinstall it from Recovery Installer after updating. The screen tells you if the plugin is missing or older.
  - A clear warning first: home screen **folders are removed and bubbles go back to the default order**. Games, apps and saves are not deleted. A second "Are you sure?" follows.
  - If a rebuild is already scheduled, you can keep it or cancel it.
  - Needs Enso (plugins loading at boot). On h-encore the plugin loads too late, so the delete is skipped and reported, never done while the home screen is using the database.
- Sony Recovery: pressing O on a confirm screen goes straight back to the list. The confusing "SUCCESS / Cancelled" screen is gone. Closing a result screen with O no longer also closes the list.
- Fixed the wide gaps in titles such as "Sony Recovery    Rebuild Database". A dash character the screen font can't draw is replaced with `-` in about 45 messages.

## v1.1.5
- Fix: after "Save Changes" in the Plugins menu, the "Changes saved... reboot to apply" message closed straight away unless X was held down. The message, plus the Clean Config and Remove Duplicates screens, now waits until you let go of X and press a button.
- **Safety fix:** a held X could skip confirmation screens. Clean Config, the Sony Recovery confirms (including the 3-step Format Memory Card and Restore System) and the Cheat Manager screens counted the X that opened them as a new press. Each screen now needs a fresh press.
- Check for Updates: the result line now reads "Installed version is up to date", "...is older than the release" or "...is newer than the release". Release tags written as `v.1.1.4` (extra dot) are now read correctly; before, they showed "newer than the release" even when the versions matched.
- Rebuild Database: when app.db can't be seen, the screen now says nothing was deleted. It shows which part of `ur0:shell/db` this app can reach, so the cause can be found.

## v1.1.4
- **Plugin toggles now really work.** In the Plugins menu, turning a plugin on or off changed a *different* plugin in config.txt whenever a section header (like `*KERNEL` or `*main`) was above it. The menu showed the change, but the config didn't get it. Each row now maps to the right plugin.
- Plugin changes now **save immediately** (like AutoPlugin2). If the save fails, the switch flips back and the error code is shown. Reboot to apply.
- **Saving no longer deletes plugin lines.** Before, a plugin listed in more than one section (for example `henkaku.suprx` under `*main`, `*NPXS10015` and `*NPXS10016`) was treated as one "duplicate", and saving deleted the other lines. Each section now keeps its own lines, and only the `#` of a changed line is touched. Blank lines, comments and line endings are kept as they were.
- Safer config writes: the first change in each session copies the original to `config.txt.bak`. Every save goes through a temporary file that is read back and checked before it replaces the config.
- "Remove Duplicates" and "Clean Config" only treat lines as duplicates inside the **same section**. Clean Config refuses to run, rather than drop lines, if the config is unusually large.
- Plugin Fix Mode: "Reset Config" now saves the old config as `config.txt.before_reset` first. Protected plugins (henkaku, storagemgr, gamesd) show their real ON/OFF state.
- **CPU Speed:** removed ARM 500 MHz (the system rejects it with error 0x802B0000 and stays at 444) and BUS 166 MHz (the system keeps 222). Old saved settings are converted automatically. The clock file now stores MHz values.
- **Sony Recovery > Rebuild Database:** shows the real error code and its meaning if app.db can't be deleted. (Superseded in v1.1.6, where the boot plugin does the rebuild.)

## v1.1.3
- Fix: the update check failed at once with "DNS lookup failed (0x80410116)" because the DNS timeout was passed in the wrong unit. It is now 5 seconds.

## v1.1.2
- **Update check now uses its own HTTPS client (mbedTLS)** instead of the Vita's built-in SSL library, which could not complete a handshake with GitHub on firmware 3.65 (errors 0x80435001, then 0x80431075).
  - Certificates are checked against 28 root certificates bundled in the app (`source/ca_roots.h`).
  - Follows GitHub's redirects and handles chunked replies; the download still goes to `ux0:downloads/` with progress and SHA-256 check.
  - Error screen shows a readable reason and, for certificate problems, the issuer that was not trusted.
  - If the Vita clock is wrong, the certificate date check is skipped and a note is shown.
- Build now needs mbedTLS: run `vdpm mbedtls` once.
- Known: the "retry without certificate check" option (Triangle) is still there; it skips verification for that attempt only.

## v1.1.1
- **New: Check for Updates** item in the main menu.
  - Asks the GitHub Releases API for the newest release of this project and compares its tag with the installed version.
  - Shows installed vs latest version, the VPK file name and size.
  - Press X to download the VPK to `ux0:downloads/` (progress bar, O cancels, `.part` file so a failed download leaves nothing broken).
  - Verifies the SHA-256 when GitHub publishes one; deletes the file on mismatch.
  - Nothing is installed automatically: install the VPK yourself with VitaShell.
  - Triangle on the error screen retries without the certificate check (off by default).
- Fix: HTTPS now starts the Vita SSL library (`sceSslInit`) before the first request. The first test build failed with error 0x80435001.
- Build: SceSsl stub is linked when the SDK has it; CMake prints a warning if not.
- Internal: network start-up is shared between the FTP server and the update screen (`net_ensure_init`).

## v1.1.0
- **System Info**
  - Firmware is read from the os0 partition when possible; if it differs from what the system reports, the reported value is shown in yellow ("system reports X").
  - Exploit row says "Enso (permanent CFW)" when Enso is detected; the "no Enso" warning is hidden in that case (also in Diagnostics).
  - New "Detection debug" section showing the raw values used for model, motherboard and firmware detection.
- **CPU Speed** (Advanced)
  - L/R triggers now change the selected value (D-pad still works).
  - The live column shows the real readback from the hardware; mismatches are yellow.
  - Error codes from refused clock changes are shown in red.
  - Default bus clock is now the stock 222 MHz (old saved default is migrated).
- Added `build.sh`: `bash build.sh`, `bash build.sh clean`, `bash build.sh bump`.
- Known issues: Vita Slim 2000 is shown as "1000 (OLED)"; firmware may show 3.73 on Enso 3.65; ARM above 444 MHz and bus 166 MHz are not applied by the system.

## v1.0.9
- FTP server: the `ftp://` address is **green** while a client is connected and **red** when no client is connected.

## v1.0.8
- Based on the v1.0.7 source.
- Title bar reads **"PS Vita Recovery Menu vX.Y.Z"** on the left and **"By DrinkingSubset"** at the far right, on every screen.
- New version system: one `VERSION` file drives the title bar and the VPK version; `bump_version.sh` raises it by one step (1.0.9 -> 1.1.0 -> 1.1.1 ...).

## Version rule
Every new build raises the version by one step: 1.0.9, 1.1.0, 1.1.1, 1.1.2 ...
