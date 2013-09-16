#pragma once

#include "common.h"

#include <connection.h>

// Sends IM to a buddy.
using SendSuccessCb = std::function<void()>;
int send_im_message(PurpleConnection* gc, const char* uid, const char* message,
                    const SendSuccessCb& success_cb = nullptr, const ErrorCb& error_cb = nullptr);

// Send typing notification.
unsigned send_typing_notification(PurpleConnection* gc, const char* uid);

// Marks messages as read.
void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids);
