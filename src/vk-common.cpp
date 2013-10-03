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


string buddy_name_from_uid(uint64 uid)
{
    return str_format("id%llu", (unsigned long long)uid);
}

uint64 uid_from_buddy_name(const char* name)
{
    assert(name[0] == 'i' && name[1] == 'd');
    return atoll(name + 2);
}
