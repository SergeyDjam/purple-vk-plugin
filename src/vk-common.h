// Common data structures for Vk.com interaction.

#pragma once

#include <connection.h>

#include "common.h"
#include "contrib/purple/http.h"

// We get connection options and store in this structure on login because we have no way
// of knowing when the account options have been changed, so we want to prevent potential
// inconsistencies. As a bonus,it is more type-safe.
struct VkConnOptions
{
    bool only_friends_in_blist;
    bool chats_in_blist;
    bool mark_as_read_online_only;
    bool mark_as_read_inactive_tab;
    bool imitate_mobile_client;
    string blist_default_group;
    string blist_chat_group;
};

// Several useful error codes
enum VkErrorCodes {
    // User authorization failed (most likely, access token has expired: redo auth).
    VK_AUTHORIZATION_FAILED = 5,
    // Too many requests per second: send a new request after a moment.
    VK_TOO_MANY_REQUESTS_PER_SECOND = 6,
    // Flood control: message with same guid already sent.
    VK_FLOOD_CONTROL = 9,
    // Something went horrible wrong
    VK_INTERNAL_SERVER_ERROR = 10,
    // Captcha needed: user sent too many requests and is required to confirm he is alive
    VK_CAPTCHA_NEEDED = 14,
    // Validation required: currently this is used when the user logins from some unusual place (e.g. another country)
    VK_VALIDATION_REQUIRED = 17
};

// Information about one user. Used mostly for "Get Info", showing buddy list tooltip etc.
// Gets periodically updated. See vk.com for documentation on each field.
struct VkUserInfo
{
    // Pair name+surname. It is saved, because we can set custom alias for the user,
    // but still display original name in "Get Info" dialog
    string real_name;

    string activity;
    string bdate;
    string domain;
    string education;
    time_t last_seen;
    string mobile_phone;
    // Both online or online_mobile can be set to true at the same time.
    bool online;
    bool online_mobile;
    string photo_min;
    string photo_max;
};

// Message, describing one received message. This structure is used for saving received messages
// until they must be marked as read.
struct VkReceivedMessage
{
    uint64 msg_id;
    uint64 user_id;
    uint64 chat_id;
};

typedef vector<VkReceivedMessage> VkReceivedMessage_vec;

// A structure, describing a previously uploaded doc. It is used to check whether the doc
// has already been uploaded before and not upload it again.
struct VkUploadedDocInfo
{
    string filename;
    uint64 size;
    string md5sum;
    string url;
};

// A structure, describing one multiuser chat. participants must include admin_id.
struct VkChatInfo
{
    uint64 admin_id;
    string title;
    uint64_vec participants;
    // We store both participant uids and participant names, so that we can get the name -> uid
    // mapping back for vk_get_cb_real_name. This map gets updated only when conversation
    // is open.
    //
    // General notes regarding names in chats: unlike users in buddy list where we can separately set
    // whichever alias we like (and we set username to idXXX and alias to real name) Pidgin does not
    // allow manually specifying aliases for chat participants (2.x versions still allow it, but 3.0
    // will forbid). We resort to somewhat complex scheme of using real name or real name + nickname/idXXX
    // as chat user name.
    map<string, uint64> participant_names;
};


// All timed events must be added via this timeout_add, because only then they will be properly
// destroyed upon closing connection.
typedef function_ptr<bool()> TimeoutCb;
void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback);


// Data, associated with account. It contains all information, required for connecting and executing
// API calls.
class VkConnData
{
public:
    VkConnData(PurpleConnection* gc, const string& email, const string& password);
    ~VkConnData();

    void authenticate(const SuccessCb& success_cb, const ErrorCb &error_cb);

    // Access token, used for accessing the API. If the token is empty, authentication is in process.
    const string& access_token() const
    {
        return m_access_token;
    }

    // User id of the authenticated user.
    uint64 self_user_id() const
    {
        return m_self_user_id;
    }

    // Connection options, initialized on login.
    const VkConnOptions& options() const
    {
        return m_options;
    }

    // Set of user ids of friends. Updated upon login and on timer by update_users. We generally do
    // not care if this is a bit outdated. This set is updated only upon login and ones per
    // some time interval.
    uint64_set friend_user_ids;

    // Set of user ids of all buddies (including friends) which the user has dialog with. This set
    // is updated upon login, ones per some time interval and each time we send message (if needed).
    uint64_set dialog_user_ids;

    // Map from user identifier to user information. All users from friend_user_ids and dialog_user_ids
    // and all chat participants must be present in this map. Gets updated periodically (once in 15 minutes).
    // Items are only added to this map and NEVER removed.
    map<uint64, VkUserInfo> user_infos;

    // Ids of all chats user participates with.
    uint64_set chat_ids;

    // Map from chat identifier to chat information. Items are only added to this map and NEVER removed.
    map<uint64, VkChatInfo> chat_infos;

    // There is a problem with processing outgoing messages: either they are sent by us and need no further
    // processing, or they are sent by some other client (or from website) and we need to at least append
    // them to log. We can potentially receive response from messages.send *after* longpoll informs us
    // about them. The devised algorithm is as follows:
    //
    // 1) Upon calling messages.send, last_msg_send_time is updated to the current time.
    // 2) The returned mid from messages.send call is stored in sent_msg_ids.
    // 3) When longpoll processes outgoing message with given mid, it checks sent_msg_ids if the message
    //    has been sent by us. If received mid is not present in sent_msg_ids, it checks if the last message
    //    has been sent by us earlier then 1 second. If it has not, after a 5 second timeout sent_msg_ids
    //    is checked again (hopefully, by that time messages.send would have returned a message and the desured
    //    mid will be in sent_msg_ids).
    //
    // Both variables refer only to *locally* sent messages.
    uint64_set sent_msg_ids;

    steady_time_point last_msg_sent_time;

    // These two sets are updated when user selects "Add buddy" or "Remove buddy" in the buddy list.
    // They are permanently stored in account properties, loaded in VkConnData constructor and stored in destructor.
    uint64_set manually_added_buddies;
    uint64_set manually_removed_buddies;

    // These set is updated when user selects "Add chat" or "Join chat" in the buddy list.
    // They are permanently stored in account properties, loaded in VkConnData constructor and stored in destructor.
    uint64_set manually_added_chats;

    // A collection of messages, which should be marked as read later (when user starts
    // typing or activates tab or changes status to Available). Must be stored and loaded, so that
    // we do not lose any read statuses.
    VkReceivedMessage_vec deferred_mark_as_read;

    // We check this collection on each file xfer and update it after upload to Vk.com. It gets stored and loaded
    // from settings.
    map<uint64, VkUploadedDocInfo> uploaded_docs;

    // We store buddy and chat names, buddy and chat groups names in order to check later if user has move buddies
    // or chats to new groups. See check_custom_alias_group(). Buddy and chat names most of the time are duplicate
    // with the data in user_infos (unless user has changed the name).
    map<uint64, string> buddy_blist_last_alias;
    map<uint64, string> chat_blist_last_alias;
    map<uint64, string> buddy_blist_last_group;
    map<uint64, string> chat_blist_last_group;

    // Unfortunately, Pidgin requires each open chat to have a unique int identifier. This vector stores mapping
    // from Vk.com chat ids to Pidgin open chat conversation ids. See more in NOTE for chat_name_from_id
    // in vk-common.cpp.
    vector<pair<int, uint64>> chat_conv_ids;

    // If true, connection is in "closing" state. This is set in vk_close and is used in longpoll
    // callback to differentiate the case of network timeout/silent connection dropping and connection
    // cancellation.
    bool is_closing() const
    {
        return m_closing;
    }

    void set_closing()
    {
        m_closing = true;
    }

    // Per-connection HTTP keepalive pool, initialized upon first HTTP connection and destroy
    // upon closing the connection.
    PurpleHttpKeepalivePool* get_keepalive_pool();

private:
    string m_email;
    string m_password;
    string m_access_token;
    uint64 m_self_user_id;

    VkConnOptions m_options;

    PurpleConnection* m_gc;
    bool m_closing;

    uint_set timeout_ids;

    PurpleHttpKeepalivePool* m_keepalive_pool;

    friend void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback);
};

inline VkConnData* get_conn_data(PurpleConnection* gc)
{
    return (VkConnData*)purple_connection_get_protocol_data(gc);
}


// Functions for converting buddy name (Pidgin) to/from user id (Vk.com).
string user_name_from_id(uint64 user_id);
// NOTE: Sometimes we can get username different from "idXXX", such as when the user has added buddy manually
// and we still have not replaced it with proper buddy. The caller code should check for this.
// If quiet is false, the function will output an error into log if it returns zero.
uint64 user_id_from_name(const char* name, bool quiet = false);

// Functions for converting chat name (stored as in blist as "id" component) to/from chat id (Vk.com).
string chat_name_from_id(uint64 chat_id);
// NOTE: We should never get names different from "chatXXX" because we are the only one who control them
// (both "id" attributes in blist and conversation names).
// If quiet is false, the function will output an error into log if it returns zero.
uint64 chat_id_from_name(const char* name, bool quiet = false);

