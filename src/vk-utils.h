// Miscellaneous Vk.com utilities.

#pragma once

#include "common.h"

#include <connection.h>

// Finds links to photo/video/docs on vk.com and returns attachment string, describing them
// as required by message.send API call.
string parse_vkcom_attachments(const string& message);

// Gets buddy names for buddies in the contact list.
string get_buddy_name(PurpleConnection* gc, uint64 uid);

// Returns true if uid is present in buddy list.
bool in_buddy_list(PurpleConnection* gc, uint64 uid);

// Map of several PurpleLogs (one for each user), so that they are not created for each received message.
// Used in vk-message-recv.cpp
class PurpleLogCache
{
public:
    PurpleLogCache(PurpleConnection* gc);
    ~PurpleLogCache();

    PurpleLog* for_uid(uint64 uid);

private:
    PurpleConnection* m_gc;
    map<uint64, PurpleLog*> m_logs;

    // Opens PurpleLog for given uid.
    PurpleLog* open_for_uid(uint64 uid);
};
