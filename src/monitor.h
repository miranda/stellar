#ifndef MONITOR_H
#define MONITOR_H
#include "stellar.h"

void extract_monitor_name(const unsigned char *edid, size_t length, char *out_name, size_t max_len);

bool monitor_disable_crtc(StellarState *st, int screen_idx);
bool monitor_restore_crtc(StellarState *st, int screen_idx);

void monitor_update_screen_info(StellarState *st, int screen_idx);
void monitor_update_all_screens(StellarState *st);
void monitor_build_screen_info_json(StellarState *st, char *buf, size_t buf_size);
void monitor_apply_rotation(StellarState *st, int screen_idx);
void monitor_apply_all_rotations(StellarState *st);
bool monitor_apply_tearfree(StellarState *st, int screen_idx);
void monitor_apply_all_tearfree(StellarState *st);

#endif
