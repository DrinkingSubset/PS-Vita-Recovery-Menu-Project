#pragma once

#define MAX_PLUGINS 64

int         plugins_load(void);
int         plugins_get_count(void);
const char *plugins_get_name(int idx);
const char *plugins_get_section(int idx);
const char *plugins_get_config_path(void);   /* path of the active tai config */
int         plugins_is_enabled(int idx);
void        plugins_toggle(int idx);
int         plugins_save(void);
int plugins_is_essential(int idx);
int plugins_remove_duplicates(void);
int plugins_clean_config(void);
int plugins_apply_safe_mode(void);  /* disable all non-essential plugins, no confirm */
