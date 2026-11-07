#include "../drc/drc.h"
drc_state_t drc_st;
uint8_t drc_flush_request;
volatile uint8_t prof_phase;
uint8_t drc_no_fast = 1;   // (no generated code here: tile writes never deferred)
