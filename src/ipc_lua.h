#ifndef IPC_LUA_H
#define IPC_LUA_H
#include "stellar.h"

bool init_lua(StellarState *st);
int make_server_socket(StellarState *st);
void accept_client(StellarState *st);
void remove_client(StellarState *st, int idx);
void consume_client_data(StellarState *st, int idx);
void broadcast_line(StellarState *st, const char *line);
bool check_lua_module_available(const char *module_name);
void lua_call_noargs(StellarState *st, const char *func);
void lua_on_pointer_screen_change(StellarState *st, int old_screen, int new_screen);
void parse_settings_from_json(StellarState *st);
void load_window_rules(StellarState *st);
RuleResult apply_window_rules(StellarState *st, Window w, const char *class_name, const char *win_name);
void update_stellar_picom_config(StellarState *st);

#endif
