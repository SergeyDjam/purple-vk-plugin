// Buddies, chats and buddy list management. This code is a real mess.

#pragma once

#include "common.h"

#include <connection.h>

// NOTE: Buddy list management.
// Currently buddy list contains three types of nodes for Vk.com account:
//  1) friends,
//  2) non-friends that the user had chat with,
//  3) multiuser chats where the user takes part.

// Updates VkConnData::friend_user_ids, ::dialog_user_ids, ::user_infos and ::chat_infos, updates buddy list.
//
// If update_presence is false, does not set presence statuses.
void update_buddies(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb = nullptr);

// Updates presence status of non-friends, which we have open conversation with.
void update_open_conversation_presence(PurpleConnection* gc);


// Checks if users are not present in buddy list and adds them to buddy list regardless
// of "Show only friends in buddy list" option. Used when receiving message from buddy etc.
//
// Always updates presence of buddies, because this function is used only for buddies not in the buddy
// list and longpoll does not receive online/offline status changes for these buddies.
void add_buddies_if_needed(PurpleConnection* gc, const uint64_set& user_ids, const SuccessCb& on_update_cb = nullptr);

// An "overload' for add_buddies_if_needed.
void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb = nullptr);

// Checks whether buddy should be present in buddy list and removes if buddy is not needed (e.g. is not a friend
// and "Show only friends in buddy list" is set). Must be called when conversation is closed (and we kept
// buddy in buddy list only because we had conversation with him).
void remove_buddy_if_needed(PurpleConnection* gc, uint64 user_id);

// Adds or updated information on buddies in VkConnData::user_infos.
//
// NOTE: user_ids must contain only non-friends.
void add_or_update_user_infos(PurpleConnection* gc, const uint64_set& user_ids, const SuccessCb& on_update_cb);


// Checks if chats are not present in buddy list and adds them to buddy list regardless of
// "Show chats in buddy list" option. Used when receiving message from chat.
void add_chats_if_needed(PurpleConnection* gc, const uint64_set& chat_ids, const SuccessCb& on_update_cb);

// An "overload" for add_chats_if_needed
void add_chat_if_needed(PurpleConnection* gc, uint64 chat_id, const SuccessCb& on_update_cb);

// Checks whether chat should be present in buddy list and removes if chat is not needed (e.g. "Show chats in buddy list"
// is set). Must be called when conversation is closed.
void remove_chat_if_needed(PurpleConnection* gc, uint64 chat_id);

// Adds or updated information on chats in VkConnData::chat_infos.
void add_or_update_chat_infos(PurpleConnection* gc, const uint64_set& chat_ids, const SuccessCb& on_update_cb);


// Updates only presence status of the given buddy in buddy list according to information in VkConnData::user_infos.
// Used in longpoll, where we directly receive online/offline status.
void update_presence_in_buddy_list(PurpleConnection* gc, uint64 user_id);


// Checks if user has manually changed alias and/or group for any of blist items (users or chats) and
// adds appropriate tags (custom-group and custom-alias). Must be called upon logout.
//
// This function is required because libpurple does not report aliasing or moving chats between groups unlike
// it does for buddies (alias_buddy and group_buddy in prpl_info). We do things uniformly for buddies and chats.
void check_custom_alias_group(PurpleConnection* gc);
