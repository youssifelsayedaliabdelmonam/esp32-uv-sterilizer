#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <Arduino.h>
#include "system_types.h"

void lcdInit();
void lcdStartTask();
void lcdUpdateSharedStatus(const SystemStatus& status);

#endif // LCD_DISPLAY_H
