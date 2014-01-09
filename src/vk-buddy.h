// Buddies and buddy list management

#pragma once

#include "common.h"

#include <connection.h>

// NOTE: Buddy list management.
// Currently buddy list contains three types of nodes for Vk.com account:
//  1) friends,
//  2) non-friends that the user had chat with,
//  3) (NOT IMPLEMENTED) multiuser chats where the user takes part.
//
// 1) Friends are always added to buddy list.
// 2) Non-friends are added either if user has "Show only friends in buddy list" account option
//    unset or if the user has IM conversation open with them. The latter is required so that
//    Pidgin shows proper user name, status and avatar. If user has "Show only friends in buddy list"
//    enabled and conversation closes, non-friend buddy is removed from blist.
// 3) Multiuser chats... TODO (same as non-friends, non-friend participants in multiuser chats are never
//    added to buddy list).

// Updates VkConnData::friend_uids and ::user_infos, updates buddy list.
//
// If update_presence is false, does not set presence statuses.
void update_buddies(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb = nullptr);

// Updates status of non-friends, which we have open conversation with.
void update_open_conv_status(PurpleConnection* gc);

// Updates VkConnData::user_infos and buddy list with users with given uids.
//
// Always updates presence of buddies, because this function is used only for buddies not in the buddy
// list and longpoll does not receive online/offline status changes for these buddies.
//
// Unaffected by "Show only friends in buddy list" and "Show chats in buddy list" account options.
void add_to_buddy_list(PurpleConnection* gc, const uint64_vec& uids, const SuccessCb& on_update_cb = nullptr);

// Checks whether buddy should be present in buddy list and removes if buddy is not needed (e.g. is not a friend
// and "no friends in buddy list has been selected).
//
// If convo_closed is true, this function is called from convo_closed handler and conversation, while still
// open, will be closed in a moment.
void remove_from_buddy_list_if_not_needed(PurpleConnection* gc, const uint64_vec& uids, bool convo_closed);

// Adds or updated information on buddies in VkConnData::user_infos
void add_or_update_user_infos(PurpleConnection* gc, const uint64_vec& uids,
                              const SuccessCb& on_update_cb = nullptr);

// Fetches buddy full name (First name + second name).
using NameFetchedCb = std::function<void(const string& data)>;
void get_user_full_name(PurpleConnection* gc, uint64 uid, const NameFetchedCb& fetch_cb);

// Finds user by "screen name" i.e. nickname.
using UidFetchedCb = std::function<void(uint64 uid)>;
void find_user_by_screenname(PurpleConnection* gc, const string& screen_name, const UidFetchedCb& fetch_cb);
