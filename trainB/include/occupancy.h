#ifndef OCCUPANCY_H
#define OCCUPANCY_H

#include <Arduino.h>

void IR_occupancy_init();
void IR_occupancy_update();

int get_count();
int get_entries();
int get_exits();

void breakbeam_read();
void loadcell_init();
float get_weight();
// float get_weight_percent();
float get_occupancy_percent(float weight, int count);

#endif