// Sending and receiving messages, marking messages as read.

#pragma once

#include "common.h"

#include <connection.h>

// Sends IM to a buddy.
using SendSuccessCb = std::function<void()>;
int send_im_message(PurpleConnection* gc, uint64 uid, const char* message,
                    const SendSuccessCb& success_cb = nullptr, const ErrorCb& error_cb = nullptr);

// Send attachment to a buddy. Used in vk-filexfer.cpp. Attachment must be a string, formed as required
// by Vk.com rules.
void send_im_attachment(PurpleConnection* gc, uint64 uid, const string& attachment);


// Send typing notification.
unsigned send_typing_notification(PurpleConnection* gc, uint64 uid);

// Marks messages as read.
void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids);

// Receives unread messages.
using ReceivedCb = std::function<void()>;
void receive_unread_messages(PurpleConnection* gc, const ReceivedCb& received_cb);

// Receives messages with given ids. Used in vk-longpoll
void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb = nullptr);
