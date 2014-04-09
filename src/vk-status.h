// Online/offline status management.

#pragma once

#include "common.h"

#include <connection.h>

// Sets account as online if status of account is online (not away/busy etc.).
// Should be called once per 15 minutes or when account status has changed.
void update_status(PurpleConnection* gc);

// Sets account as online. Added for symmetry with set_offline.
void set_online(PurpleConnection* gc);

// Sets account as offline. Should be called when disconnecting.
void set_offline(PurpleConnection* gc);
