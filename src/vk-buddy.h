// Buddies, chats and buddy list management. This code is a real mess.

#pragma once

#include "common.h"

#include <connection.h>

// NOTE: Buddy list management.
// Currently buddy list contains three types of nodes for Vk.com account:
//  1) friends,
//  2) non-friends that the user had chat with,
//  3) multiuser chats where the user takes part.
//
// Even if options such as "Show only friends in buddy list" are enabled,
// all the users which we currently have conversation with must be present
// in the buddy list (otherwise we will not be able to show avatar or display
// proper name).

// Updates list of friends, user information, chat information and updates buddy list correspondingly.
// Updates presence for non-friends.
void update_user_chat_infos(PurpleConnection* gc);

// Updates presence of friends. Must be used before longpoll starts receiving updates.
void update_friends_presence(PurpleConnection* gc, const SuccessCb& on_update_cb);

// Updates presence status of non-friends, which we have open conversation with.
void update_open_conv_presence(PurpleConnection* gc);

// Adds or updates information on chats.
void update_user_infos(PurpleConnection* gc, const set<uint64>& user_ids, const SuccessCb& on_update_cb);

// Adds or updates information on chats. If update_blist is true, corresponding buddy list node
// is updated too if it exists.
void update_chat_infos(PurpleConnection* gc, const set<uint64>& chat_ids, const SuccessCb& on_update_cb,
                       bool update_blist = false);


// Updates only presence status of the given buddy in buddy list according to information in user_infos.
// Longpoll updates user_infos directly for friends.
void update_presence_in_blist(PurpleConnection* gc, uint64 user_id);


// Checks if users are not present in buddy list and adds them to buddy list (regardless
// of account options). Used when receiving message from user (user must be present in the buddy list).
void add_buddies_if_needed(PurpleConnection* gc, const set<uint64>& user_ids, const SuccessCb& on_update_cb = nullptr);

// An overload for add_buddies_if_needed.
void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb = nullptr);

// Checks whether buddy should be present in buddy list and removes if it should not be present. Must be called
// when the conversation is closed.
void remove_buddy_if_needed(PurpleConnection* gc, uint64 user_id);


// Checks if chats are not present in buddy list and adds them to buddy list (regardless of
// account options). Used when receiving chat message (chat must be present in the buddy list).
void add_chats_if_needed(PurpleConnection* gc, const set<uint64>& chat_ids, const SuccessCb& on_update_cb);

// An overload for add_chats_if_needed
void add_chat_if_needed(PurpleConnection* gc, uint64 chat_id, const SuccessCb& on_update_cb);

// Checks whether chat should be present in buddy list and removes if it should not be present. Must be
// called when the conversation is closed.
void remove_chat_if_needed(PurpleConnection* gc, uint64 chat_id);


// Stores current version of buddy list in VkData::blist_buddies and ::blist_chats. Must
// be called before all other functions upon login.
void check_blist_on_login(PurpleConnection* gc);

// Checks if user has manually changed alias and/or group for any of blist items (users or chats) and
// adds appropriate tags (custom-group and custom-alias). Must be called upon logout.
void check_blist_on_logout(PurpleConnection* gc);
