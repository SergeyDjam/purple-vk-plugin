// Buddies and buddy list management

#pragma once

#include "common.h"

#include <connection.h>

// NOTE: Buddy list management.
// Currently buddy list contains three types of nodes for Vk.com account:
//  1) friends,
//  2) non-friends that the user had chat with,
//  3) (NOT IMPLEMENTED) multiuser chats where the user takes part.

// Updates VkConnData::friend_uids and ::user_infos, updates buddy list.
//
// If update_presence is false, does not set presence statuses.
void update_buddies(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb = nullptr);

// Updates presence status of non-friends, which we have open conversation with.
void update_open_conversation_presence(PurpleConnection* gc);


// Checks if users are not present in buddy list and adds them to VkConnData::user_infos
// and buddy list regardless of "Show only friends in buddy list" and "Show chats in buddy list"
// options. Used when opening conversation to buddy etc.
//
// Always updates presence of buddies, because this function is used only for buddies not in the buddy
// list and longpoll does not receive online/offline status changes for these buddies.
void add_buddies_if_needed(PurpleConnection* gc, const uint64_vec& uids, const SuccessCb& on_update_cb = nullptr);

// An overload for add_to_buddy_list.
void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb = nullptr);

// Checks whether buddy should be present in buddy list and removes if buddy is not needed (e.g. is not a friend
// and "no friends in buddy list has been selected). Must be called when conversation is closed (and we kept
// buddy in buddy list only because we had conversation with him).
void remove_buddy_if_needed(PurpleConnection* gc, uint64 user_id);


// Adds or updated information on buddies in VkConnData::user_infos.
//
// NOTE: uids must contain only non-friend uids.
void add_or_update_user_infos(PurpleConnection* gc, const uint64_vec& user_ids, const SuccessCb& on_update_cb = nullptr);


// Updates only presence status of the given buddy in buddy list according to information in VkConnData::user_infos.
// Used in longpoll, where we directly receive online/offline status.
void update_presence_in_buddy_list(PurpleConnection* gc, uint64 user_id);


// Set account alias to user full name (first name + second name).
void set_account_alias(PurpleConnection* gc);

// Finds user by "screen name" i.e. nickname.
typedef std::function<void(uint64 uid)> UidFetchedCb;
void find_user_by_screenname(PurpleConnection* gc, const string& screen_name, const UidFetchedCb& fetch_cb);
