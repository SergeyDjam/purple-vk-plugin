#include <debug.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"

#include "vk-utils.h"


void set_account_alias(PurpleConnection* gc)
{
    uint64 user_id = get_data(gc).self_user_id();
    vkcom_debug_info("Getting full name for %" PRIu64 "\n", user_id);

    CallParams params = { {"user_ids", to_string(user_id)}, {"fields", "first_name,last_name"} };
    vk_call_api(gc, "users.get", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }

        const picojson::array& users = result.get<picojson::array>();
        if (users.size() != 1) {
            vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }

        if (!field_is_present<string>(users[0], "first_name") || !field_is_present<string>(users[0], "last_name")) {
            vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }
        string first_name = users[0].get("first_name").get<string>();
        string last_name = users[0].get("last_name").get<string>();

        string full_name = first_name + " " + last_name;
        PurpleAccount* account = purple_connection_get_account(gc);
        purple_account_set_alias(account, full_name.data());
    }, nullptr);
}


namespace
{

// We match on all URLs, beginning with http[s]://vk.com/ and containing photoXXX_YYY or videoXXX_YYY
// because there are too many ways to open photo/video in vk.com: from search, from newsfeed etc.

// Such regex. Very \\\\. Much ?
// Boy, I suck at this.
const char attachment_regex_const[] = "https?://vk.com/\\S*?(?<attachment>(photo|video|doc|wall)-?\\d*_\\d*)"
                                      "\\S*?(\\Whash=(?<hash>\\w+))?";

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
            vkcom_debug_error("Unable to compile message attachment regexp, aborting");
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
PurpleBuddy* buddy_from_user_id(PurpleConnection* gc, uint64 user_id)
{
    string who = user_name_from_id(user_id);
    PurpleAccount* account = purple_connection_get_account(gc);
    return purple_find_buddy(account, who.data());
}

} // End of anonymous namespace

string get_user_display_name(PurpleConnection* gc, uint64 user_id)
{
    PurpleBuddy* buddy = buddy_from_user_id(gc, user_id);
    if (buddy)
        return purple_buddy_get_alias(buddy);
    VkUserInfo* info = get_user_info(gc, user_id);
    if (info)
        return info->real_name;
    else
        return user_name_from_id(user_id);
}

bool user_in_buddy_list(PurpleConnection* gc, uint64 user_id)
{
    return buddy_from_user_id(gc, user_id) != nullptr;
}

bool user_is_friend(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).friend_user_ids, user_id);
}

bool had_dialog_with_user(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).dialog_user_ids, user_id);
}

bool is_unknown_user(PurpleConnection* gc, uint64 user_id)
{
    return !contains(get_data(gc).user_infos, user_id);
}

bool have_conversation_with_user(PurpleConnection* gc, uint64 user_id)
{
    string who = user_name_from_id(user_id);
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who.data(),
                                                 purple_connection_get_account(gc)) != nullptr;
}

bool chat_in_buddy_list(PurpleConnection* gc, uint64 chat_id)
{
    return find_purple_chat_by_id(gc, chat_id) != nullptr;
}

bool participant_in_chat(PurpleConnection* gc, uint64 chat_id)
{
    return contains(get_data(gc).chat_ids, chat_id);
}

bool is_unknown_chat(PurpleConnection* gc, uint64 chat_id)
{
    return !contains(get_data(gc).chat_infos, chat_id);
}

bool have_open_chat(PurpleConnection* gc, uint64 chat_id)
{
    string name = chat_name_from_id(chat_id);
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, name.data(),
                                                 purple_connection_get_account(gc)) != nullptr;
}


VkUserInfo* get_user_info(PurpleBuddy* buddy)
{
    PurpleConnection* gc = purple_account_get_connection(purple_buddy_get_account(buddy));
    uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
    return get_user_info(gc, user_id);
}

VkUserInfo* get_user_info(PurpleConnection* gc, uint64 user_id)
{
    if (user_id == 0)
        return nullptr;

    VkData& gc_data = get_data(gc);
    auto it = gc_data.user_infos.find(user_id);
    if (it == gc_data.user_infos.end())
        return nullptr;
    return &it->second;
}

VkChatInfo* get_chat_info(PurpleConnection* gc, uint64 chat_id)
{
    if (chat_id == 0)
        return nullptr;

    VkData& gc_data = get_data(gc);
    auto it = gc_data.chat_infos.find(chat_id);
    if (it == gc_data.chat_infos.end())
        return nullptr;
    return &it->second;
}

bool is_user_manually_added(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).manually_added_buddies(), user_id);
}

bool is_user_manually_removed(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).manually_removed_buddies(), user_id);
}

bool is_chat_manually_added(PurpleConnection* gc, uint64 chat_id)
{
    return contains(get_data(gc).manually_added_chats, chat_id);
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

PurpleLog* PurpleLogCache::for_user(uint64 user_id)
{
    if (contains(m_logs, user_id)) {
        return m_logs[user_id];
    } else {
        PurpleLog* log = open_for_user_id(user_id);
        m_logs[user_id] = log;
        return log;
    }
}

PurpleLog* PurpleLogCache::for_chat(uint64 chat_id)
{
    if (contains(m_chat_logs, chat_id)) {
        return m_chat_logs[chat_id];
    } else {
        PurpleLog* log = open_for_chat_id(chat_id);
        m_chat_logs[chat_id] = log;
        return log;
    }
}

PurpleLog* PurpleLogCache::open_for_user_id(uint64 user_id)
{
    string buddy = user_name_from_id(user_id);
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
        fetched_cb(map<uint64, VkGroupInfo>());
        return;
    }

    string group_ids_str = str_concat_int(',', group_ids);
    vkcom_debug_info("Getting infos for groups %s\n", group_ids_str.data());

    CallParams params = { {"group_ids", group_ids_str} };
    vk_call_api(gc, "groups.getById", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            fetched_cb(map<uint64, VkGroupInfo>());
            return;
        }

        map<uint64, VkGroupInfo> infos;
        const picojson::array& groups = result.get<picojson::array>();
        for (const picojson::value& v: groups) {
            if (!field_is_present<double>(v, "id") || !field_is_present<string>(v, "name")
                    || !field_is_present<string>(v, "type")) {
                vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                                   result.serialize().data());
                fetched_cb(map<uint64, VkGroupInfo>());
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
    }, [=](const picojson::value&) {
        fetched_cb(map<uint64, VkGroupInfo>());
    });
}

string get_user_href(uint64 user_id, const VkUserInfo& info)
{
    if (!info.domain.empty())
        return str_format("<a href='https://vk.com/%s'>%s</a>", info.domain.data(), info.real_name.data());
    else
        return str_format("<a href='https://vk.com/id%" PRIu64 "'>%s</a>", user_id, info.real_name.data());
}

string get_group_href(uint64 group_id, const VkGroupInfo& info)
{
    if (!info.screen_name.empty())
        return str_format("<a href='https://vk.com/%s'>%s</a>", info.screen_name.data(), info.name.data());
    // How the fuck am I supposed to learn these URL patterns?
    if (info.type == "group") {
        return str_format("<a href='https://vk.com/club%" PRIu64 "'>%s</a>", group_id, info.name.data());
    } else if (info.type == "page") {
        return str_format("<a href='https://vk.com/public%" PRIu64 "'>%s</a>", group_id, info.name.data());
    } else if (info.type == "event") {
        return str_format("<a href='https://vk.com/event%" PRIu64 "'>%s</a>", group_id, info.name.data());
    } else {
        vkcom_debug_error("Unknown group types %s\n", info.type.data());
        return "https://vk.com";
    }
}


PurpleConversation* find_conv_for_id(PurpleConnection* gc, uint64 user_id, uint64 chat_id)
{
    if (user_id != 0)
        return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, user_name_from_id(user_id).data(),
                                                     purple_connection_get_account(gc));
    else
        return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chat_name_from_id(chat_id).data(),
                                                     purple_connection_get_account(gc));
}


void resolve_screen_name(PurpleConnection* gc, const char* screen_name, const ResolveScreenNameCb& resolved_cb)
{
    CallParams params = { { "screen_name", screen_name } };
    vk_call_api(gc, "utils.resolveScreenName", params, [=](const picojson::value& result) {
        if (!field_is_present<string>(result, "type") || !field_is_present<double>(result, "object_id")) {
            vkcom_debug_error("Strange response from resolveScreenName: %s\n",
                               result.serialize().data());
            resolved_cb("", 0);
            return;
        }

        resolved_cb(result.get("type").get<string>(), result.get("object_id").get<double>());
    }, [=](const picojson::value&) {
        resolved_cb("", 0);
    });
}


vector<PurpleChat*> find_all_purple_chats(PurpleAccount* account)
{
    vector<PurpleChat*> chats;
    for (PurpleBlistNode* node = purple_blist_get_root(); node; node = purple_blist_node_next(node, FALSE)) {
        if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
            PurpleChat* chat = PURPLE_CHAT(node);
            if (purple_chat_get_account(chat) == account)
                chats.push_back(chat);
        }
    }

    return chats;
}

PurpleChat* find_purple_chat_by_id(PurpleConnection* gc, uint64 chat_id)
{
    vector<PurpleChat*> chats = find_all_purple_chats(purple_connection_get_account(gc));
    for (PurpleChat* chat: chats) {
        const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
        if (!chat_name)
            continue;

        if (chat_id_from_name(chat_name) == chat_id)
            return chat;
    }

    return nullptr;
}


void find_user_by_screenname(PurpleConnection* gc, const string& screen_name, const UserIdFetchedCb& fetch_cb)
{
    vkcom_debug_info("Finding user id for %s\n", screen_name.data());

    CallParams params = { {"screen_name", screen_name} };
    vk_call_api(gc, "utils.resolveScreenName", params, [=](const picojson::value& result) {
        if (!field_is_present<string>(result, "type") || !field_is_present<double>(result, "object_id")) {
            vkcom_debug_error("Unable to find user matching %s\n", screen_name.data());
            fetch_cb(0);
            return;
        }

        if (result.get("type").get<string>() != "user") {
            vkcom_debug_error("Type of %s is %s\n", screen_name.data(),
                               result.get("type").get<string>().data());
            fetch_cb(0);
            return;
        }

        uint64 user_id = result.get("object_id").get<double>();
        fetch_cb(user_id);
    }, [=](const picojson::value&) {
        fetch_cb(0);
    });
}
