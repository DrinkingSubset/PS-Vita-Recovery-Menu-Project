#pragma once

typedef enum {
    RESTORE_OK            =  0,
    RESTORE_RUNNING       =  1,
    RESTORE_ERR_IO        = -1,
    RESTORE_ERR_NO_BACKUP = -2,
    RESTORE_ERR_KERNEL    = -3,
} RestoreStatus;

/* Initialisation */
void restore_ensure_dirs(void);

/* Function 1 — Disable every plugin in config.txt for a clean boot */
RestoreStatus restore_safe_mode(void);

/* Function 2 — Overwrite config.txt with known-good minimal defaults */
RestoreStatus restore_reset_tai_config(void);

/* Function 3a — Back up ux0:tai/ to ux0:data/vita_recovery/tai_backup/ */
RestoreStatus restore_backup_tai(void);

/* Function 3b — Restore ux0:tai/ from the backup */
RestoreStatus restore_restore_tai(void);

/* Function 4 — Delete corrupt app.db so firmware regenerates LiveArea */
RestoreStatus restore_rebuild_livearea(void);

/* Query helpers */
const char   *restore_get_status_msg(void);
RestoreStatus restore_get_last_status(void);
int           restore_backup_exists(void);
