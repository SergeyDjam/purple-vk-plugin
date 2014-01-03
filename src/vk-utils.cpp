#include <debug.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
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

namespace
{

// Returns either PurpleBuddy or nullptr.
PurpleBuddy* buddy_from_uid(PurpleConnection* gc, uint64 uid)
{
    string who = buddy_name_from_uid(uid);
    PurpleAccount* account = purple_connection_get_account(gc);
    return purple_find_buddy(account, who.data());
}

} // End of anonymous namespace

string get_buddy_name(PurpleConnection* gc, uint64 uid)
{
    VkConnData* conn_data = get_conn_data(gc);
    PurpleBuddy* buddy = buddy_from_uid(gc, uid);
    if (buddy)
        return purple_buddy_get_alias(buddy);
    else if (contains_key(conn_data->user_infos, uid))
        return conn_data->user_infos[uid].name;
    else
        return buddy_name_from_uid(uid);
}

bool in_buddy_list(PurpleConnection* gc, uint64 uid)
{
    return buddy_from_uid(gc, uid) != nullptr;
}

bool is_friend(PurpleConnection* gc, uint64 uid)
{
    VkConnData* conn_data = get_conn_data(gc);
    return contains_key(conn_data->friend_uids, uid);
}

bool is_unknown_uid(PurpleConnection* gc, uint64 uid)
{
    VkConnData* conn_data = get_conn_data(gc);
    return !contains_key(conn_data->user_infos, uid);
}

bool have_conversation_with(PurpleConnection* gc, uint64 uid)
{
    string who = buddy_name_from_uid(uid);
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who.data(),
                                                 purple_connection_get_account(gc)) != nullptr;

}


VkUserInfo* get_user_info_for_buddy(PurpleBuddy* buddy)
{
    PurpleConnection* gc = purple_account_get_connection(purple_buddy_get_account(buddy));
    VkConnData* conn_data = get_conn_data(gc);
    uint64 uid = uid_from_buddy_name(purple_buddy_get_name(buddy));
    if (!contains_key(conn_data->user_infos, uid))
        return nullptr;
    return &conn_data->user_infos[uid];
}

VkUserInfo* get_user_info_for_buddy(PurpleConnection* gc, const char* name)
{
    uint64 uid = uid_from_buddy_name(name);
    return get_user_info_for_buddy(gc, uid);
}

VkUserInfo* get_user_info_for_buddy(PurpleConnection* gc, uint64 user_id)
{
    VkConnData* conn_data = get_conn_data(gc);
    if (!contains_key(conn_data->user_infos, user_id))
        return nullptr;
    return &conn_data->user_infos[user_id];
}


PurpleLogCache::PurpleLogCache(PurpleConnection* gc)
    : m_gc(gc)
{
}

PurpleLogCache::~PurpleLogCache()
{
    for (pair<uint64, PurpleLog*> p: m_logs)
        purple_log_free(p.second);
}

PurpleLog* PurpleLogCache::for_uid(uint64 uid)
{
    if (contains_key(m_logs, uid)) {
        return m_logs[uid];
    } else {
        PurpleLog* log = open_for_uid(uid);
        m_logs[uid] = log;
        return log;
    }
}

PurpleLog*PurpleLogCache::for_chat(uint64 chat_id)
{
    if (contains_key(m_chat_logs, chat_id)) {
        return m_chat_logs[chat_id];
    } else {
        PurpleLog* log = open_for_chat_id(chat_id);
        m_chat_logs[chat_id] = log;
        return log;
    }
}

PurpleLog* PurpleLogCache::open_for_uid(uint64 uid)
{
    string buddy = buddy_name_from_uid(uid);
    PurpleAccount* account = purple_connection_get_account(m_gc);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, buddy.data(), account);
    return purple_log_new(PURPLE_LOG_IM, buddy.data(), account, conv, time(nullptr), nullptr);
}

PurpleLog* PurpleLogCache::open_for_chat_id(uint64 chat_id)
{
    string name = chat_name_from_id(chat_id);
    PurpleAccount* account = purple_connection_get_account(m_gc);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, name.data(), account);
    return purple_log_new(PURPLE_LOG_CHAT, name.data(), account, conv, time(nullptr), nullptr);
}


// List of emoji-common (yes, common for the sample of one) smiley pairs:
//   UTF-16   (UTF-8)                 Text smiley
//   D83DDE0A (0xF0 0x9F 0x98 0x8A)   :-)
//   D83DDE03 (0xF0 0x9F 0x98 0x83)   :-D
//   D83DDE09 (0xF0 0x9F 0x98 0x89)   ;-)
//   D83DDE0B (0xF0 0x9F 0x98 0x8B)   :-P
//   D83DDE12 (0xF0 0x9F 0x98 0x92)   :-(
//   D83DDE22 (0xF0 0x9F 0x98 0xA2)   :'(
//   D83DDE29 (0xF0 0x9F 0x98 0xA9)   :((
//   2764     (0xE2 0x9D 0xA4)        <3
//   D83DDE1A (0xF0 0x9F 0x98 0x9A)   :-*
// All other emoji are left intact => will be displayed as Unicode symbols. We need to download all emoji
// and provide proper custom smiley, but I generally hate graphical smileys, so am waiting for anyone
// willing to do this.
void replace_emoji_with_text(string& message)
{
    str_replace(message, "\xE2\x9D\xA4", "&lt;3");

    // All other emoji conveniently start with same prefix
    size_t pos = 0;
    while ((pos = message.find("\xF0\x9F\x98", pos)) != string::npos) {
        const char* smiley = nullptr;
        switch ((unsigned char)message[pos + 3]) {
        case 0x8A:
            smiley = ":-)";
            break;
        case 0x83:
            smiley = ":-D";
            break;
        case 0x89:
            smiley = ";-)";
            break;
        case 0x8B:
            smiley = ":-P";
            break;
        case 0x92:
            smiley = ":-(";
            break;
        case 0xA2:
            smiley = ":'(";
            break;
        case 0xA9:
            smiley = ":((";
            break;
        case 0x9A:
            smiley = ":-*";
            break;
        }
        if (smiley)
            message.replace(pos, 4, smiley);
        pos += 3;
    }
}


void get_groups_info(PurpleConnection* gc, uint64_vec group_ids, const GroupInfoFetchedCb& fetched_cb)
{
    if (group_ids.empty()) {
        fetched_cb({});
        return;
    }

    string group_ids_str = str_concat_int(',', group_ids);
    purple_debug_info("prpl-vkcom", "Getting infos for groups %s\n", group_ids_str.data());

    CallParams params = { {"group_ids", group_ids_str} };
    vk_call_api(gc, "groups.getById", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            fetched_cb({});
            return;
        }

        map<uint64, VkGroupInfo> infos;
        const picojson::array& groups = result.get<picojson::array>();
        for (const picojson::value& v: groups) {
            if (!field_is_present<double>(v, "id") || !field_is_present<string>(v, "name")
                    || !field_is_present<string>(v, "type")) {
                purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                                   result.serialize().data());
                fetched_cb({});
                return;
            }

            uint64 id = v.get("id").get<double>();
            VkGroupInfo& info = infos[id];

            info.name = v.get("name").get<string>();
            info.type = v.get("type").get<string>();
            if (field_is_present<string>(v, "screen_name"))
                info.screen_name = v.get("screen_name").get<string>();
        }
        fetched_cb(infos);
    });
}

string get_user_href(uint64 user_id, const VkUserInfo& info)
{
    if (!info.domain.empty())
        return str_format("<a href='http://vk.com/%s'>%s</a>", info.domain.data(), info.name.data());
    else
        return str_format("<a href='http://vk.com/id%lld'>%s</a>", (long long)user_id, info.name.data());
}

string get_group_href(uint64 group_id, const VkGroupInfo& info)
{
    if (!info.screen_name.empty())
        return str_format("<a href='http://vk.com/%s'>%s</a>", info.screen_name.data(), info.name.data());
    // How the fuck am I supposed to learn these URL patterns?
    if (info.type == "group") {
        return str_format("<a href='http://vk.com/club%lld'>%s</a>", (long long)group_id, info.name.data());
    } else if (info.type == "page") {
        return str_format("<a href='http://vk.com/public%lld'>%s</a>", (long long)group_id, info.name.data());
    } else if (info.type == "event") {
        return str_format("<a href='http://vk.com/event%lld'>%s</a>", (long long)group_id, info.name.data());
    } else {
        purple_debug_error("prpl-vkcom", "Unknown group types %s\n", info.type.data());
        return "http://vk.com";
    }
}
