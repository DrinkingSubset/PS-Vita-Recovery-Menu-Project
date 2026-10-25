# Changelog — PS Vita Recovery Menu

By DrinkingSubset. Newest version first. Copy a version's block straight into
the GitHub release notes.

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
