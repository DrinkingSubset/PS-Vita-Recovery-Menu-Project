# Changelog — PS Vita Recovery Menu

By DrinkingSubset. Newest version first. Copy a version's block straight into
the GitHub release notes.

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
- **Sony Recovery > Rebuild Database:** shows the real error code and its meaning if app.db can't be deleted, and backs up app.db to `ux0:data/VitaRecovery/app.db.bak` first. It also removes leftover journal files and checks that the delete really happened. Sony's own Safe Mode is suggested if it's blocked.

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
