#ifndef HVAC_H
#define HVAC_H

#include <Arduino.h>

void HVAC_init();
void taskHvac();
void handleSerialCommands();
void HVAC_loop();

void setOccupancy(int occ);

#endif