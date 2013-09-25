#pragma once

#include "common.h"

#include <connection.h>

// Initializes and starts PurpleXfer for trasnferring document to particular user.
PurpleXfer* new_xfer(PurpleConnection* gc, uint64 uid);
