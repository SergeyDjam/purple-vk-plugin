#include <cstdlib>

#include <debug.h>

#include "vk-auth.h"

#include "vk-common.h"

const char VK_CLIENT_ID[] = "3833170";

namespace {

// Splits the comma-separated string of integers.
uint64_set str_split_int(const char* str)
{
    uint64_set ret;
    while (*str) {
        char* next;
        uint64 i = strtoll(str, &next, 10);
        ret.insert(i);
        if (*next) {
            assert(*next == ',');
            str = next + 1;
        } else {
            str = next;
        }
    }
    return ret;
}

} // namespace

VkConnData::VkConnData(PurpleConnection* gc, const string& email, const string& password)
    : m_email(email),
      m_password(password),
      m_gc(gc),
      m_closing(false)
{
    PurpleAccount* account = purple_connection_get_account(m_gc);
    const char* str = purple_account_get_string(account, "manually_added_buddies", "");
    manually_added_buddies = str_split_int(str);

    str = purple_account_get_string(account, "manually_removed_buddies", "");
    manually_removed_buddies = str_split_int(str);

    str = purple_account_get_string(account, "deferred_mark_as_read", "");
    deferred_mark_as_read = str_split_int(str);
}

VkConnData::~VkConnData()
{
    PurpleAccount* account = purple_connection_get_account(m_gc);
    string str = str_concat_int(',', manually_added_buddies);
    purple_account_set_string(account, "manually_added_buddies", str.data());

    str = str_concat_int(',', manually_removed_buddies);
    purple_account_set_string(account, "manually_removed_buddies", str.data());

    str = str_concat_int(',', deferred_mark_as_read);
    purple_account_set_string(account, "deferred_mark_as_read", str.data());
}

void VkConnData::authenticate(const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    vk_auth_user(m_gc, m_email, m_password, VK_CLIENT_ID, "friends,photos,audio,video,docs,messages",
        [=](const string& access_token, const string& uid) {
            m_access_token = access_token;
            try {
                m_uid = stoi(uid);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-fpermissive" // catch (...) makes GCC 4.7.2 return strange error, fixed in later GCCs
#endif
            } catch (...) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
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
    if (strncmp(name, "id", 2) != 0)
        return 0;
    return atoll(name + 2);
}

string chat_name_from_id(uint64 chat_id)
{
    return str_format("chat%llu", (unsigned long long)chat_id);
}

uint64 chat_id_from_name(const char* name)
{
    if (strncmp(name, "chat", 4) != 0)
        return 0;
    return atoll(name + 4);
}
