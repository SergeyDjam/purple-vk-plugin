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

// Returns true if uid is a friend.
bool is_friend(PurpleConnection* gc, uint64 uid);

// Returns true if uid is not present even in user infos.
bool is_unknown_uid(PurpleConnection* gc, uint64 uid);

// Returns true if we are currently having an open IM conversation with (chats ignored).
bool have_conversation_with(PurpleConnection* gc, uint64 uid);

// Returns VkUserInfo, corresponding to buddy.
VkUserInfo* get_user_info_for_buddy(PurpleBuddy* buddy);
VkUserInfo* get_user_info_for_buddy(PurpleConnection* gc, const char* name);
VkUserInfo* get_user_info_for_buddy(PurpleConnection* gc, uint64 user_id);

// Check if buddy has been manually added to the buddy list.
bool is_manually_added(PurpleConnection* gc, uint64 user_id);

// Check if buddy has been manually removed from the buddy list.
bool is_manually_removed(PurpleConnection* gc, uint64 user_id);

// Map of several PurpleLogs (one for each user), so that they are not created for each received message.
// Used in vk-message-recv.cpp
class PurpleLogCache
{
public:
    PurpleLogCache(PurpleConnection* gc);
    ~PurpleLogCache();

    // Opens PurpleLog for given uid or returns an already open one.
    PurpleLog* for_uid(uint64 uid);
    // Opens PurpleLog for given chat id or returns an already open one.
    PurpleLog* for_chat(uint64 chat_id);

private:
    PurpleConnection* m_gc;
    map<uint64, PurpleLog*> m_logs;
    map<uint64, PurpleLog*> m_chat_logs;

    PurpleLog* open_for_uid(uint64 uid);
    PurpleLog* open_for_chat_id(uint64 chat_id);
};

// Replaces most common emoji with smileys: :), :(, :'( etc. Custom smileys are left intact
// (as Unicode symbols).
void replace_emoji_with_text(string& message);

// Returns some information about group, used when receiving reposts of group messages.
struct VkGroupInfo
{
    string name;
    string type;
    string screen_name;
};

typedef std::function<void(const map<uint64, VkGroupInfo>& infos)> GroupInfoFetchedCb;
void get_groups_info(PurpleConnection* gc, uint64_vec group_ids, const GroupInfoFetchedCb& fetched_cb);

// Gets href, which points to the user page.
string get_user_href(uint64 user_id, const VkUserInfo& info);

// Gets href, which points to the group page.
string get_group_href(uint64 group_id, const VkGroupInfo& info);

// Finds conversation open with user_id or chat_id. Either of the two identifiers must be zero.
PurpleConversation* find_conv_for_id(PurpleConnection* gc, uint64 user_id, uint64 chat_id);

// Resolves screen name, like a nickname or group name to type and identifier.
typedef std::function<void(const string& type, uint64 id)> ResolveScreenNameCb;
void resolve_screen_name(PurpleConnection* gc, const char* screen_name, const ResolveScreenNameCb& resolved_cb);
