// Receiving messages, marking messages as read.

#pragma once

#include "common.h"

#include <connection.h>

using ReceivedCb = std::function<void()>;

// Receives all not received messages. If account is connected for the first time, only unread messages
// are received, otherwise receive all messages (both sent and received) since last login. Used
// in vk-longpoll.cpp.
void receive_messages(PurpleConnection* gc, const ReceivedCb& received_cb);

// Receives messages with given ids. Used in vk-longpoll.cpp
void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb = nullptr);

// Marks messages as read.
void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids);
