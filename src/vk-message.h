#pragma once

#include "common.h"

#include <connection.h>

using SendSuccessCb = std::function<void(const string& uid)>;

// Sends IM to a buddy.
void send_im_message(PurpleConnection* gc, const string& uid, const string& message, SendSuccessCb success_cb,
                     ErrorCb error_cb);
