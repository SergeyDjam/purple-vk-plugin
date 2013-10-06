// Buddies and buddy list management

#pragma once

#include "common.h"

#include <connection.h>

// NOTE: Currently buddy list contains three types of nodes for Vk.com account:
//  1) friends,
//  2) non-friends that the user had chat with,
//  3) (NOT IMPLEMENTED) multichats where the user takes part.

// Adds, updates or removes buddies from the buddy list.
//
// If update_presence is true, presence information will be updated. update_presence should be true
// only when called before starting Long Poll processing.
void update_buddy_list(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb = nullptr);

// Adds or updates information on buddy with given uid. We always update presence of buddies, because
// this function is used only for buddies not in the friend list and longpoll does not receive online/offline
// status changes for these buddies.
void update_buddies(PurpleConnection* gc, const uint64_vec& uids, const SuccessCb& on_update_cb = nullptr);

// Fetches buddy full name (First name + second name).
using NameFetchedCb = std::function<void(const string& data)>;
void get_buddy_full_name(PurpleConnection* gc, uint64 uid, const NameFetchedCb& fetch_cb);
