#pragma once

#include "common.h"

#include <connection.h>

using OnUpdateCb = std::function<void(void)>;

// Adds, updates or removes buddies.
void update_buddy_list(PurpleConnection* gc, const OnUpdateCb& on_update_cb = nullptr);

// Adds or updates information on buddy with given id (not buddy name, without prepended "id"!).
void update_buddy(PurpleConnection* gc, const string& id, const OnUpdateCb& on_update_cb = nullptr);

// Fetches buddy full name (First name + second name).
using FetchCb = std::function<void(const string& data)>;
void get_buddy_full_name(PurpleConnection* gc, const string& id, FetchCb success_cb);
