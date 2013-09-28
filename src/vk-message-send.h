// Sending messages, sending typing notifications.

#pragma once

#include "common.h"

#include <connection.h>

// Sends IM to a buddy.
using SendSuccessCb = std::function<void()>;
int send_im_message(PurpleConnection* gc, uint64 uid, const char* raw_message,
                    const SendSuccessCb& success_cb = nullptr, const ErrorCb& error_cb = nullptr);

// Send attachment to a buddy. Used in vk-filexfer.cpp. Attachment must be a string, formed as required
// by Vk.com rules.
void send_im_attachment(PurpleConnection* gc, uint64 uid, const string& attachment);

// Send typing notification.
unsigned send_typing_notification(PurpleConnection* gc, uint64 uid);
