// Miscellaneous Vk.com utilities.

#pragma once

#include "common.h"

#include <connection.h>

#include "vk-common.h"

// Finds links to photo/video/docs on vk.com and returns attachment string, describing them as required
// by message.send API call.
string parse_vkcom_attachments(const string& message);

// Gets buddy names for buddies in the contact list.
string get_buddy_name(PurpleConnection* gc, uint64 uid);

// Returns true if uid is present in buddy list.
bool in_buddy_list(PurpleConnection* gc, uint64 uid);

// Returns true if we are currently having an open IM conversation with (chats ignored).
bool have_conversation_with(PurpleConnection* gc, uint64 uid);

// Returns VkUserInfo, corresponding to buddy.
VkUserInfo* get_user_info_for_buddy(PurpleBuddy* buddy);
VkUserInfo* get_user_info_for_buddy(PurpleConnection* gc, const char* name);

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

// Replaces most common emoji with smileys: :), :(, :'( etc. Custom smileys are left intact
// (as Unicode symbols).
void replace_emoji_with_text(string& message);
