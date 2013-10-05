// Buddies and buddy list management

#pragma once

#include "common.h"

#include <connection.h>

// Adds, updates or removes buddies.
// If update_presence is true, presence information will be updated. update_presence should be true
// only when called from Long Poll processing.
void update_buddy_list(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb = nullptr);

// Adds or updates information on buddy with given uid. See update_buddy_list on update_presence.
void update_buddy(PurpleConnection* gc, uint64 uid, const SuccessCb& on_update_cb = nullptr,
                  bool update_presence = false);

// Fetches buddy full name (First name + second name).
using NameFetchedCb = std::function<void(const string& data)>;
void get_buddy_full_name(PurpleConnection* gc, uint64 uid, const NameFetchedCb& fetch_cb);
