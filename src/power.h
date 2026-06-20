#ifndef POWER_H
#define POWER_H
#include "stellar.h"

void check_idle_screens(StellarState *st);
void reset_idle_timer(StellarState *st);
void start_stellar_saver(StellarState *st);
void stop_stellar_saver(StellarState *st);

#endif
