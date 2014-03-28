// Chat management functions.
#pragma once

#include "common.h"

#include <connection.h>

// Returns conversation id (Pidgin), matching given chat id (Vk.com), 0 if no conversation open.
int chat_id_to_conv_id(PurpleConnection* gc, uint64 chat_id);

// Returns chat id (Vk.com), matching given conversation id (Pidgin), 0 if conversation does not match
// any chat.
uint64 conv_id_to_chat_id(PurpleConnection* gc, int conv_id);

// Adds unique conversation id for newly allocated conversation. Does not check if chat_id is already
// present.
int add_new_conv_id(PurpleConnection* gc, uint64 chat_id);

// Removes conversation id upon closing conversation.
void remove_conv_id(PurpleConnection* gc, int conv_id);


// Opens new chat conversation. If chat is already open, does nothing.
void open_chat_conv(PurpleConnection* gc, uint64 chat_id, const SuccessCb& success_cb);

// Checks, which conversations are already open and add them to VkConnData::chat_conv_ids.
// Should be called on login.
void check_open_chat_convs(PurpleConnection* gc);

// Updates chat conversation for conv_id the information from VkConnData::chat_infos
// (updates user names etc.)
void update_open_chat_conv(PurpleConnection* gc, int conv_id);

// Updates all open chat conversations (see update_open_chat_conv).
void update_all_open_chat_convs(PurpleConnection* gc);

// Returns user id for user named who in open chat conv_id.
uint64 find_user_id_from_conv(PurpleConnection* gc, int conv_id, const char* who);
