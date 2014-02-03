// Common data structures for Vk.com interaction.

#pragma once

#include <connection.h>

#include "common.h"
#include "contrib/purple/http.h"

// Several useful error codes
enum VkErrorCodes {
    // User authorization failed (most likely, access token has expired: redo auth).
    VK_AUTHORIZATION_FAILED = 5,
    // Too many requests per second: send a new request after a moment.
    VK_TOO_MANY_REQUESTS_PER_SECOND = 6,
    // Flood control: message with same guid already sent.
    VK_FLOOD_CONTROL = 9,
    // Captcha needed: user sent too many requests and is required to confirm he is alive
    VK_CAPTCHA_NEEDED = 14,
    // Validation required: currently this is used when the user logins from some unusual place (e.g. another country)
    VK_VALIDATION_REQUIRED = 17
};

// Information about one user. Used mostly for "Get Info", showing buddy list tooltip etc.
// Gets periodically updated. See vk.com for documentation on each field.
struct VkUserInfo
{
    // name is saved, because we can set custom alias for the user, but still display original name
    // in "Get Info" dialog
    string name;

    string activity;
    string bdate;
    string domain;
    string education;
    time_t last_seen;
    string mobile_phone;
    // Either online or online_mobile can be set to true (or both).
    bool online;
    bool online_mobile;
    string photo_min;
    string photo_max;
};

// A structure, describing a previously uploaded doc. It is used to check whether the doc
// has already been uploaded before and not upload it again.
struct VkUploadedDoc
{
    uint64 id;
    string filename;
    uint64 size;
    string md5sum;
};

// Data, associated with account. It contains all information, required for connecting and executing
// API calls.
class VkConnData
{
public:
    VkConnData(PurpleConnection* gc, const string& email, const string& password);
    ~VkConnData();

    void authenticate(const SuccessCb& success_cb, const ErrorCb& error_cb = nullptr);

    const string& access_token() const
    {
        return m_access_token;
    }

    uint64 uid() const
    {
        return m_uid;
    }

    // Set of uids of friends. Updated upon login and on timer by update_users. We generally do
    // not care if this is a bit outdated.
    uint64_set friend_uids;

    // Map from user identifier to user information. All users from buddy list must be present
    // in this map.
    // Gets updated periodically (once in 15 minutes).
    map<uint64, VkUserInfo> user_infos;

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

    // A collection of message ids, which should be marked as read later (when user starts
    // typing or changes status to Available). Must be stored and loaded, so that we do not
    // lose any read statuses.
    uint64_set deferred_mark_as_read;

    // We check this collection on each file xfer and update it after upload to Vk.com. It gets stored and loaded
    // from settings.
    vector<VkUploadedDoc> uploaded_docs;

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

    // We need to remove all timed events added by timeout_add upon closing connection or the crash
    // is possible otherwise. This set stores all ids of the events.
    uint_set timeout_ids;

    // Per-connection HTTP keepalive pool, initialized upon first HTTP connection and destroy
    // upon closing the connection.
    PurpleHttpKeepalivePool* keepalive_pool = nullptr;

private:
    string m_email;
    string m_password;
    string m_access_token;
    uint64 m_uid;

    PurpleConnection* m_gc;
    bool m_closing;
};

inline VkConnData* get_conn_data(PurpleConnection* gc)
{
    return (VkConnData*)purple_connection_get_protocol_data(gc);
}

// Functions for converting buddy name to/from uid.
string buddy_name_from_uid(uint64 uid);
uint64 uid_from_buddy_name(const char* name);

// Functions for converting chat name to/from chat id.
string chat_name_from_id(uint64 uid);
uint64 chat_id_from_name(const char* name);
