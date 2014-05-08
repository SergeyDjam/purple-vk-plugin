// Miscellaneous Vk.com utilities.

#pragma once

#include "common.h"

#include <connection.h>

#include "vk-common.h"


// Sets account alias to user full name (first name + second name).
void set_account_alias(PurpleConnection* gc);

// Finds links to photo/video/docs on vk.com and returns attachment string, describing them as required
// by message.send API call.
string parse_vkcom_attachments(const string& message);

// Gets display name for user.
string get_user_display_name(PurpleConnection* gc, uint64 user_id);

// Gets display name for user in chat.
string get_user_display_name(PurpleConnection *gc, uint64 user_id, uint64 chat_id);

// Gets display name for self in chats (with " (you)" appended).
string get_self_chat_display_name(PurpleConnection* gc);

// Gets unique display name for user, used when user has duplicate name with other user in chat,
// appends some unique id.
string get_unique_display_name(PurpleConnection* gc, uint64 user_id);

// Returns true if user_id is present in buddy list.
bool user_in_buddy_list(PurpleConnection* gc, uint64 user_id);

// Returns true if user_id is a friend.
bool is_user_friend(PurpleConnection* gc, uint64 user_id);

// Returns true if user ever had a dialog with user_id.
bool had_dialog_with_user(PurpleConnection* gc, uint64 user_id);

// Returns true if user_id is not present even in user infos.
bool is_unknown_user(PurpleConnection* gc, uint64 user_id);

// Returns true if we currently have an open IM conversation with (chats ignored).
bool have_conversation_with_user(PurpleConnection* gc, uint64 user_id);

// Returns trus if chat is present in buddy list.
bool chat_in_buddy_list(PurpleConnection* gc, uint64 chat_id);

// Returns true if user is a participant in chat_id.
bool is_participant_in_chat(PurpleConnection* gc, uint64 chat_id);

// Returns true if chat is not present even in chat infos.
bool is_unknown_chat(PurpleConnection* gc, uint64 chat_id);

// Returns VkUserInfo, corresponding to buddy or nullptr if info still has not been added.
VkUserInfo* get_user_info(PurpleBuddy* buddy);
VkUserInfo* get_user_info(PurpleConnection* gc, uint64 user_id);

// Returns VkChatInfo or nullptr if info still has not been added.
VkChatInfo* get_chat_info(PurpleConnection* gc, uint64 chat_id);

// Checks if buddy has been manually added to the buddy list.
bool is_user_manually_added(PurpleConnection* gc, uint64 user_id);

// Checks if buddy has been manually removed from the buddy list.
bool is_user_manually_removed(PurpleConnection* gc, uint64 user_id);

// Checks if chat has been manually added to the buddy list.
bool is_chat_manually_added(PurpleConnection* gc, uint64 chat_id);

// Checks if chat has manually removed from the buddy list.
bool is_chat_manually_removed(PurpleConnection* gc, uint64 chat_id);

// Map of several PurpleLogs (one for each user), so that they are not created for each received message.
// Used in vk-message-recv.cpp
class PurpleLogCache
{
public:
    PurpleLogCache(PurpleConnection* gc);
    ~PurpleLogCache();

    // Opens PurpleLog for given user_id or returns an already open one.
    PurpleLog* for_user(uint64 user_id);
    // Opens PurpleLog for given chat id or returns an already open one.
    PurpleLog* for_chat(uint64 chat_id);

private:
    PurpleConnection* m_gc;
    map<uint64, PurpleLog*> m_logs;
    map<uint64, PurpleLog*> m_chat_logs;

    PurpleLog* open_for_user_id(uint64 user_id);
    PurpleLog* open_for_chat_id(uint64 chat_id);
};

// Replaces most common emoji with smileys: :), :(, :'( etc. Custom smileys are left intact
// (as Unicode symbols).
void replace_emoji_with_text(string& message);

// Returns true if group_id is not present even in group infos or group info is stale.
bool is_unknown_group(PurpleConnection* gc, uint64 group_id);

// Returns VkGroupInfo or nullptr if info still has not been added.
VkGroupInfo* get_group_info(PurpleConnection* gc, uint64 group_id);

// Updates information about groups in VkData.
void update_groups_info(PurpleConnection* gc, vector<uint64> group_ids, const SuccessCb& success_cb);

// Gets href, which points to the user page.
string get_user_href(uint64 user_id, const VkUserInfo& info);

// Gets href, which points to the group page.
string get_group_href(uint64 group_id, const VkGroupInfo& info);

// Finds conversation open with user_id or chat_id. Either of the two identifiers must be zero.
PurpleConversation* find_conv_for_id(PurpleConnection* gc, uint64 user_id, uint64 chat_id);

// Resolves screen name, like a nickname or group name to type and identifier.
typedef function_ptr<void(const string& type, uint64 id)> ResolveScreenNameCb;
void resolve_screen_name(PurpleConnection* gc, const char* screen_name, const ResolveScreenNameCb& resolved_cb);


// A function analogous to purple_find_buddies but which returns all chats in buddy list.
vector<PurpleChat*> find_all_purple_chats(PurpleAccount* account);

// Returns chat in buddy list, which has this chat id or null if no chat found.
PurpleChat* find_purple_chat_by_id(PurpleConnection* gc, uint64 chat_id);


// Finds user by "screen name" i.e. nickname.
typedef function_ptr<void(uint64 user_id)> UserIdFetchedCb;
void find_user_by_screenname(PurpleConnection* gc, const string& screen_name, const UserIdFetchedCb& fetch_cb);

// Determines, if the given string is an id, string in format "idXXXX" or a short name and runs func with the id
// as a parameter (or zero if searching for user failed).
template<typename Func>
void call_func_for_user(PurpleConnection* gc, const char* name, Func func)
{
    uint64 user_id = atoll(name);
    if (user_id != 0) {
        func(user_id);
        return;
    }
    user_id = user_id_from_name(name);
    if (user_id != 0) {
        func(user_id);
        return;
    }
    find_user_by_screenname(gc, name, func);
}
