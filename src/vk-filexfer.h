// File uploads.

#pragma once

#include "common.h"

#include <connection.h>

// Initializes and starts PurpleXfer for trasnferring document to particular user. Used for "Send File".
PurpleXfer* new_xfer(PurpleConnection* gc, uint64 user_id);
