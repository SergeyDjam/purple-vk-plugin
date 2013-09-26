// Long poll: receiving messages and user status changes.

#pragma once

#include <connection.h>

// Long Poll in Vk.com terminology is a server, which pushes different events to you.
// It is used for getting chat messages, online/offline user status (but not status text etc.)
// and user typing notifications.

// Initiates connection to Long Poll server and processes retrieved events. Long Poll update
// loop terminates with termination of all HTTP connections, associated with gc.
void start_long_poll(PurpleConnection* gc);
