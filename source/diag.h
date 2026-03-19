#pragma once
/*
 * diag.h — Boot Diagnostic Mode
 *
 * One-shot scan of all common soft-brick causes:
 *   - Mount point availability (ux0, ur0, imc0, uma0, grw0)
 *   - tai config presence and active path
 *   - HENkaku / taiHEN key files
 *   - Storage plugin detection (StorageMgr, YAMT, gamesd, usbmc)
 *   - storage_config.txt presence when storage plugin detected
 *   - Plugin count sanity check
 *   - Ensō status
 *
 * Results are stored in a static array and rendered as a scrollable list.
 * Severity levels: OK (green) / INFO (dim) / WARN (yellow) / FAIL (red)
 * A suggested-fix section appears at the bottom for any WARN or FAIL items.
 */

void diag_run(void);
