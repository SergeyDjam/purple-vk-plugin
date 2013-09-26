#include "vk-common.h"

#include <debug.h>

#include "vk-auth.h"

const char VK_CLIENT_ID[] = "3833170";

VkConnData::VkConnData(const string& email, const string& password)
    : m_email(email),
      m_password(password),
      m_closing(false)
{
}

void VkConnData::authenticate(PurpleConnection* gc, const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
{
    vk_auth_user(gc, m_email, m_password, VK_CLIENT_ID, "friends,photos,audio,video,docs,messages",
        [=](const string& access_token, const string& uid) {
            m_access_token = access_token;
            try {
                m_uid = stoi(uid);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-fpermissive" // catch (...) makes GCC 4.7.2 return strange error, fixed in later GCCs
            } catch (...) {
#pragma GCC diagnostic pop
                purple_debug_error("prpl-vkcom", "Error converting uid %s to integer\n", uid.c_str());
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

namespace
{

// We match on all URLS, beginning with http[s]://vk.com/ and containing photoXXX_YYY or videoXXX_YYY
// because there are too many ways to open photo/video in vk.com: from search, from newsfeed etc.
#define VK_COM_URL(type) "https?://vk.com/\\S*(?<attachment>" type "-?\\d*_\\d*)\\S*?(\\Whash=(?<hash>\\w+))?"
const char attachment_regex_const[] = VK_COM_URL("photo") "|" VK_COM_URL("video") "|" VK_COM_URL("doc");

GRegex* attachment_regex = nullptr;

} // End of anonymous namespace

bool init_vkcom_regexps()
{
    attachment_regex = g_regex_new(attachment_regex_const, GRegexCompileFlags(G_REGEX_OPTIMIZE | G_REGEX_DUPNAMES),
                                   GRegexMatchFlags(0), nullptr);
    if (!attachment_regex) {
        purple_debug_error("prpl-vkcom", "Unable to compile message attachment regexp, aborting");
        return false;
    }
    return true;
}

void destroy_vkcom_regexps()
{
    g_regex_unref(attachment_regex);
}

string parse_vkcom_attachments(const char* message)
{
    GMatchInfo* match_info;
    if (!g_regex_match(attachment_regex, message, GRegexMatchFlags(0), &match_info))
        return string();

    string ret;
    while (g_match_info_matches(match_info)) {
        if (!ret.empty())
            ret += ',';

        gchar* attachment = g_match_info_fetch_named(match_info, "attachment");
        ret += attachment;
        g_free(attachment);
        gchar* hash = g_match_info_fetch_named(match_info, "hash");
        printf("got hash: %s\n", hash);
        if (hash) {
            ret += '_';
            ret += hash;
        }
        g_free(hash);

        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    return ret;
}
