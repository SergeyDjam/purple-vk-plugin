// Common data structures for Vk.com interaction.

#pragma once

#include <map>
#include <set>

using std::map;
using std::pair;
using std::set;

#include <connection.h>

#include "common.h"
#include "contrib/purple/http.h"

// We get connection options and store in this structure on login because we have no way
// of knowing when the account options have been changed, so we want to prevent potential
// inconsistencies. As a bonus,it is more type-safe.
struct VkOptions
{
    bool only_friends_in_blist;
    bool chats_in_blist;
    bool mark_as_read_online_only;
    bool mark_as_read_replying_only;
    bool imitate_mobile_client;
    bool enable_webkit_workarounds;
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
    // Validation required: currently this is used when the user logins from some unusual place
    // (e.g. another country)
    VK_VALIDATION_REQUIRED = 17
};

// Information about one user. Used mostly for "Get Info", showing buddy list tooltip etc.
// Gets periodically updated. See vk.com for documentation on each field.
struct VkUserInfo
{
    // Pair name+surname. It is saved, because we can set custom alias for the user,
    // but still display original name in "Get Info" dialog.
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
    // We store both participant uids and participant names, because we can get two users with the
    // same real_name in one chat and no way to differentiate them --- no avatars etc.). We use
    // real name + nickname or real name + id if we have two users with equal real names.
    map<uint64, string> participants;
};

// A structure, describing one group.
struct VkGroupInfo
{
    string name;
    string type;
    string screen_name;
    // Group information is not updated periodically, only on demand, so we store the timepoint
    // of last update.
    steady_time_point last_updated;
};

// A structure, which holds the previous state of node in buddy list. Motivation: we store
// the previous version of buddy list in order to check whether the user has changed anything
// (e.g. aliased buddy, moved buddy to another group, removed buddy, aliased chat, moved chat
// to another group, removed chat). While we can get notifications from libpurple for
// the first three events, libpurple does not report changes for chats, so we do both buddies
// and chats uniformly.
struct VkBlistNode
{
    string alias;
    string group;
};

// All timed events must be added via this timeout_add, because only then they will be properly
// destroyed upon closing connection.
typedef function_ptr<bool()> TimeoutCb;
void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback);


// Data, associated with account. It contains all information, required for connecting and executing
// API calls.
class VkData
{
public:
    VkData(PurpleConnection* gc, const string& email, const string& password);
    ~VkData();

    DISABLE_COPYING(VkData)

    // Perform authentication. access_token is set upon successful authentication.
    // Authentication is performed only if access_token is empty, otherwise
    // success_cb is called immediately.
    void authenticate(const SuccessCb& success_cb, const ErrorCb &error_cb);

    // Access token, used for accessing the API.
    const string& access_token() const
    {
        return m_access_token;
    }

    void clear_access_token()
    {
        m_access_token.clear();
    }

    // User id of the authenticated user.
    uint64 self_user_id() const
    {
        return m_self_user_id;
    }

    // Connection options, initialized on login.
    const VkOptions& options() const
    {
        return m_options;
    }

    // Set of user ids of friends. Updated upon login and on timer by update_users. We generally do
    // not care if this is a bit outdated. This set is updated only upon login and ones per
    // some time interval.
    set<uint64> friend_user_ids;

    // Set of user ids of all buddies (including friends) which the user has dialog with. This set
    // is updated upon login, ones per some time interval and each time we send message (if needed).
    set<uint64> dialog_user_ids;

    // Map from user identifier to user information. All users from friend_user_ids and dialog_user_ids
    // and all chat participants must be present in this map. Gets updated periodically (once in 15 minutes).
    // Items are only added to this map and NEVER removed.
    map<uint64, VkUserInfo> user_infos;

    // Set of ids of all chats user participates with.
    set<uint64> chat_ids;

    // Map from chat identifier to chat information. Items are only added to this map and NEVER removed.
    map<uint64, VkChatInfo> chat_infos;

    // Map from group identifier to group information. Items are added on demand and get
    // updated only when info is re-requested and is stale.
    map<uint64, VkGroupInfo> group_infos;

    // There is a problem with processing outgoing messages: either they are sent by us and need no further
    // processing, or they are sent by some other client (or from website) and we need to at least append
    // them to log. We can potentially receive response from messages.send *after* longpoll informs us
    // about them. The devised algorithm is as follows:
    //
    // 1) Upon calling messages.send, m_last_msg_send_time is updated to the current time.
    // 2) The returned mid from messages.send call is stored in m_sent_msg_ids.
    // 3) When longpoll processes outgoing message with given mid, it checks m_sent_msg_ids if the message
    //    has been sent by us. If received mid is not present in m_sent_msg_ids, it checks if the last message
    //    has been sent by us earlier then 1 second. If it has not, after a 5 second timeout m_sent_msg_ids
    //    is checked again (hopefully, by that time messages.send would have returned a message and the desured
    //    mid will be in m_sent_msg_ids).
    //
    // We only to *locally* sent messages.

    // Adds sent msg id. Must be used when sending the message succeeds and we get the msg id.
    void add_sent_msg_id(uint64 msg_id)
    {
        m_sent_msg_ids.insert(msg_id);
    }

    // Checks if msg_id has been sent and removes it from the list of sent msg ids. Returns false
    // if msg_id had not been sent, true otherwise.
    bool remove_sent_msg_id(uint64 msg_id)
    {
        return m_sent_msg_ids.erase(msg_id) > 0;
    }

    // Returns send time of last locally sent message.
    steady_time_point last_msg_sent_time() const
    {
        return m_last_msg_sent_time;
    }

    // Sets last message sent time. Must be used when sending the message.
    void set_last_msg_sent_time(steady_time_point sent_time)
    {
        if (sent_time < m_last_msg_sent_time) {
            vkcom_debug_error("Trying to set last sent time earlier than currently set time\n");
            return;
        }
        m_last_msg_sent_time = sent_time;
    }

    // These two sets (manually_added_buddies and manually_removed_buddies) are updated when user selects
    // "Add buddy" or "Remove" in the buddy list. They are permanently stored in account properties,
    // loaded in VkData constructor and stored in destructor.
    const set<uint64>& manually_added_buddies() const
    {
        return m_manually_added_buddies;
    }

    const set<uint64>& manually_removed_buddies() const
    {
        return m_manually_removed_buddies;
    }

    // Adds user_id to manually added buddy list.
    void set_manually_added_buddy(uint64 user_id)
    {
        m_manually_added_buddies.insert(user_id);
        m_manually_removed_buddies.erase(user_id);
    }

    // Adds user_id to manually removed buddy list.
    void set_manually_removed_buddy(uint64 user_id)
    {
        m_manually_removed_buddies.insert(user_id);
        m_manually_added_buddies.erase(user_id);
    }

    // These two sets (manually_added_chats and manually_removed_chats) are updated when user selects "Add chat",
    // "Join chat" or "Remove" in the buddy list. They are permanently stored in account properties, loaded
    // in VkData constructor and stored in destructor.
    const set<uint64>& manually_added_chats() const
    {
        return m_manually_added_chats;
    }

    const set<uint64>& manually_removed_chats() const
    {
        return m_manually_removed_chats;
    }

    // Adds chat_id to manually added chat list.
    void set_manually_added_chat(uint64 chat_id)
    {
        m_manually_added_chats.insert(chat_id);
        m_manually_removed_chats.erase(chat_id);
    }

    // Adds chat_id to manually removed chat list.
    void set_manually_removed_chat(uint64 chat_id)
    {
        m_manually_removed_chats.insert(chat_id);
        m_manually_added_chats.erase(chat_id);
    }

    // A collection of messages, which should be marked as read later (when user starts
    // typing or activates tab or changes status to Available). Must be stored and loaded, so that
    // we do not lose any read statuses.
    vector<VkReceivedMessage> deferred_mark_as_read;

    // We check this collection on each file xfer and update it after upload to Vk.com. It gets stored and loaded
    // from settings.
    map<uint64, VkUploadedDocInfo> uploaded_docs;

    // The following two maps store the previous version of buddy list. See comments on VkBlistNode
    // for more info.
    map<uint64, VkBlistNode> blist_buddies;
    map<uint64, VkBlistNode> blist_chats;

    // Unfortunately, Pidgin requires each open chat to have a unique int identifier. This vector stores mapping
    // from Vk.com chat ids to Pidgin open chat conversation ids. See more in NOTE for chat_name_from_id
    // in vk-common.cpp.
    // This container should be changed into bimap.
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

    // Returns true if authentication is in process (try requesting access token after
    // a while).
    bool is_authenticating() const
    {
        return m_access_token.empty();
    }

    // Per-connection HTTP keepalive pool, initialized upon first HTTP connection and destroy
    // upon closing the connection.
    PurpleHttpKeepalivePool* get_keepalive_pool();

private:
    string m_email;
    string m_password;
    string m_access_token;
    uint64 m_self_user_id;

    VkOptions m_options;

    set<uint64> m_sent_msg_ids;
    steady_time_point m_last_msg_sent_time;

    set<uint64> m_manually_added_buddies;
    set<uint64> m_manually_removed_buddies;
    set<uint64> m_manually_added_chats;
    set<uint64> m_manually_removed_chats;

    PurpleConnection* m_gc;
    bool m_closing;

    set<unsigned> timeout_ids;

    PurpleHttpKeepalivePool* m_keepalive_pool;

    friend void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback);
};

inline VkData& get_data(PurpleConnection* gc)
{
    return *(VkData*)purple_connection_get_protocol_data(gc);
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

