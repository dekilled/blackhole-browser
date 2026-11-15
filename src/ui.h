#ifndef BH_UI_H
#define BH_UI_H

#include "browser.h"

/* Opens a window and runs the event loop until the user quits. Returns the exit code. */
int ui_x11_run(Browser *b, const char *initial_url);

bool browser_shortcut(Browser *b, char letter, int mods);

#endif
