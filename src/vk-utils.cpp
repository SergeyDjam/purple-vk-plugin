#include <debug.h>

#include "httputils.h"
#include "vk-common.h"

#include "vk-utils.h"

namespace
{

// We match on all URLs, beginning with http[s]://vk.com/ and containing photoXXX_YYY or videoXXX_YYY
// because there are too many ways to open photo/video in vk.com: from search, from newsfeed etc.

#define VK_COM_URL(type) "https?://vk.com/\\S*(?<attachment>" type "-?\\d*_\\d*)\\S*?(\\Whash=(?<hash>\\w+))?"
const char attachment_regex_const[] = VK_COM_URL("photo") "|" VK_COM_URL("video") "|" VK_COM_URL("doc");

} // End of anonymous namespace

string parse_vkcom_attachments(const string& message)
{
    static GRegex* attachment_regex = nullptr;
    static OnExit attachment_regex_deleter([=] {
        g_regex_unref(attachment_regex);
    });

    if (!attachment_regex) {
        attachment_regex = g_regex_new(attachment_regex_const, GRegexCompileFlags(G_REGEX_OPTIMIZE | G_REGEX_DUPNAMES),
                                       GRegexMatchFlags(0), nullptr);
        if (!attachment_regex) {
            purple_debug_error("prpl-vkcom", "Unable to compile message attachment regexp, aborting");
            return "";
        }
    }
    GMatchInfo* match_info;
    if (!g_regex_match(attachment_regex, message.data(), GRegexMatchFlags(0), &match_info))
        return "";

    string ret;
    while (g_match_info_matches(match_info)) {
        if (!ret.empty())
            ret += ',';

        char* attachment = g_match_info_fetch_named(match_info, "attachment");
        ret += attachment;
        g_free(attachment);
        char* hash = g_match_info_fetch_named(match_info, "hash");
        if (hash) {
            ret += '_';
            ret += hash;
        }
        g_free(hash);

        g_match_info_next(match_info, nullptr);
    }
    g_match_info_free(match_info);
    return ret;
}


string get_buddy_name(PurpleConnection* gc, uint64 uid)
{
    string who = buddy_name_from_uid(uid);
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, who.data());
    if (buddy)
        return purple_buddy_get_alias(buddy);
    else
        return who;
}


LogCache::LogCache(PurpleConnection* gc)
    : m_gc(gc)
{
}

LogCache::~LogCache()
{
    for (pair<uint64, PurpleLog*> p: m_logs)
        purple_log_free(p.second);
}

PurpleLog* LogCache::for_uid(uint64 uid)
{
    if (contains_key(m_logs, uid)) {
        return m_logs[uid];
    } else {
        PurpleLog* log = open_for_uid(uid);
        m_logs[uid] = log;
        return log;
    }
}

PurpleLog* LogCache::open_for_uid(uint64 uid)
{
    string buddy = buddy_name_from_uid(uid);
    PurpleAccount* account = purple_connection_get_account(m_gc);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, buddy.data(), account);
    return purple_log_new(PURPLE_LOG_IM, buddy.data(), account, conv, time(nullptr), nullptr);
}
