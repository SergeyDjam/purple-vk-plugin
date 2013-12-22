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
    // True if we can write a message to the user.
    bool can_write;

    string activity;
    string bdate;
    string domain;
    string education;
    bool is_mobile;
    time_t last_seen;
    string mobile_phone;
    bool online;
    string photo_min;
    string photo_max;
};

// Data, associated with account. It contains all information, required for connecting and executing
// API calls.
class VkConnData
{
public:
    VkConnData(PurpleConnection* gc, const string& email, const string& password);

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
