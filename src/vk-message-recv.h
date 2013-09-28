// Receiving messages, marking messages as read.

#pragma once

#include "common.h"

#include <connection.h>

// Receives unread messages. Used in vk-longpoll.cpp
using ReceivedCb = std::function<void()>;
void receive_unread_messages(PurpleConnection* gc, const ReceivedCb& received_cb);

// Receives messages with given ids. Used in vk-longpoll.cpp
void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb = nullptr);

// Marks messages as read.
void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids);
