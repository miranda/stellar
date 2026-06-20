#ifndef XDND_H
#define XDND_H
#include "stellar.h"

void init_xdnd_atoms(StellarState *st);
void create_xdnd_proxy_windows(StellarState *st);
void xdnd_proxy_cleanup(StellarState *st);
bool xdnd_proxy_start(StellarState *st, int source_screen, int target_screen, int warp_x, int warp_y, Time event_time);
void xdnd_proxy_motion(StellarState *st, int root_x, int root_y, Time timestamp);
void xdnd_proxy_drop(StellarState *st, Time timestamp);
void xdnd_proxy_handle_selection_request(StellarState *st, XSelectionRequestEvent *req);
bool xdnd_proxy_handle_client_message(StellarState *st, XClientMessageEvent *cm);

#endif
