// Online/offline status management.

#pragma once

#include "common.h"

#include <connection.h>

// Sets account as online if status of account is online (not away/busy etc.).
// Should be called once per 15 minutes or when account status has changed.
void vk_update_status(PurpleConnection* gc);

// Sets account as offline. Should be called when disconnecting.
void vk_set_offline(PurpleConnection* gc);
