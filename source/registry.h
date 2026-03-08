#pragma once

#define REG_TYPE_INT    0
#define REG_TYPE_STR    1

typedef struct {
    const char *reg_path;
    const char *reg_key;
    int         type;
    const char *label;
    int         current_val;
} RegHack;

void      registry_load_all(void);
int       registry_get_count(void);
RegHack  *registry_get(int idx);
int       registry_toggle(int idx);  /* returns 0 on success, negative on failure */
