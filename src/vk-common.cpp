#include "vk-common.h"

#include <debug.h>

#include "vk-auth.h"

const char VK_CLIENT_ID[] = "3833170";

VkConnData::VkConnData(PurpleConnection* gc, const string& email, const string& password)
    : m_email(email),
      m_password(password),
      m_gc(gc),
      m_closing(false)
{
    PurpleAccount* account = purple_connection_get_account(m_gc);
    m_last_msg_id = purple_account_get_int(account, "last_msg_id", 0);
}

void VkConnData::authenticate(const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
{
    vk_auth_user(m_gc, m_email, m_password, VK_CLIENT_ID, "friends,photos,audio,video,docs,messages",
        [=](const string& access_token, const string& uid) {
            m_access_token = access_token;
            try {
                m_uid = stoi(uid);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-fpermissive" // catch (...) makes GCC 4.7.2 return strange error, fixed in later GCCs
            } catch (...) {
#pragma GCC diagnostic pop
                purple_debug_error("prpl-vkcom", "Error converting uid %s to integer\n", uid.data());
                if (error_cb)
                    error_cb();
            }
            success_cb();
        }, error_cb);
}

void VkConnData::set_last_msg_id(uint64 msg_id)
{
    if (msg_id > m_last_msg_id) {
        m_last_msg_id = msg_id;
        // PurpleAccount has 5 second timeout before storing the properties, so it should be fine
        // calling set_int immediately after processing each message.
        purple_account_set_int(purple_connection_get_account(m_gc), "last_msg_id", m_last_msg_id);
    } else {
        purple_debug_error("prpl-vkcom", "Trying to set last msg id %llu earlier than an already received"
                           "one %llu\n", (long long unsigned)msg_id, (long long unsigned)m_last_msg_id);
    }
}


string buddy_name_from_uid(uint64 uid)
{
    return str_format("id%llu", (unsigned long long)uid);
}

uint64 uid_from_buddy_name(const char* name)
{
    assert(name[0] == 'i' && name[1] == 'd');
    return atoll(name + 2);
}
