/*
 * Hooks the application gives to the drivers. No hardware headers here, so
 * the flash-storage modules can also be built and tested on a PC.
 */
#ifndef APP_HOOKS_H
#define APP_HOOKS_H

/* Called from every wait loop: refreshes the watchdog and services USB. */
void Board_Idle(void);

#endif
