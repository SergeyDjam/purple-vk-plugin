#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"

#include "vk-utils.h"


void set_account_alias(PurpleConnection* gc)
{
    uint64 user_id = get_data(gc).self_user_id();
    vkcom_debug_info("Getting full name for %llu\n", (unsigned long long)user_id);

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


string get_user_display_name(PurpleConnection* gc, uint64 user_id, uint64 chat_id)
{
    VkChatInfo* info = get_chat_info(gc, chat_id);
    if (!info)
        return get_user_display_name(gc, user_id);

    auto it = info->participants.find(user_id);
    if (it == info->participants.end())
        return get_user_display_name(gc, user_id);
    else
        return it->second;
}

string get_self_chat_display_name(PurpleConnection* gc)
{
    const char* self_alias = purple_account_get_alias(purple_connection_get_account(gc));
    return str_format(i18n("%s (you)"), self_alias);
}

string get_unique_display_name(PurpleConnection* gc, uint64 user_id)
{
    VkUserInfo* info = get_user_info(gc, user_id);
    if (!info)
        return user_name_from_id(user_id);

    // Return either "Name (nickname)" or "Name (id)"
    if (!info->domain.empty())
        return str_format("%s (%s)", info->real_name.data(), info->domain.data());
    else
        return str_format("%s (%llu)", info->real_name.data(), (unsigned long long)user_id);
}

bool user_in_buddy_list(PurpleConnection* gc, uint64 user_id)
{
    return buddy_from_user_id(gc, user_id) != nullptr;
}

bool is_user_friend(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).friend_user_ids, user_id);
}

bool had_dialog_with_user(PurpleConnection* gc, uint64 user_id)
{
    return contains(get_data(gc).dialog_user_ids, user_id);
}

bool is_unknown_user(PurpleConnection* gc, uint64 user_id)
{
    VkUserInfo* info = get_user_info(gc, user_id);
    if (!info)
        return true;
    // We added user_infos entry in update_friends_presence but still haven't updated
    // it with real information.
    if (info->real_name.empty())
        return true;
    return false;
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

bool is_participant_in_chat(PurpleConnection* gc, uint64 chat_id)
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
    return map_at_ptr(get_data(gc).user_infos, user_id);
}

VkChatInfo* get_chat_info(PurpleConnection* gc, uint64 chat_id)
{
    if (chat_id == 0)
        return nullptr;
    return map_at_ptr(get_data(gc).chat_infos, chat_id);
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
    return contains(get_data(gc).manually_added_chats(), chat_id);
}

bool is_chat_manually_removed(PurpleConnection* gc, uint64 chat_id)
{
    return contains(get_data(gc).manually_removed_chats(), chat_id);
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


bool is_unknown_group(PurpleConnection* gc, uint64 group_id)
{
    VkGroupInfo* info = get_group_info(gc, group_id);
    if (!info)
        return true;
    steady_time_point now = steady_clock::now();
    if (to_seconds(now - info->last_updated) > 15 * 60)
        return true;
    return false;
}

VkGroupInfo* get_group_info(PurpleConnection* gc, uint64 group_id)
{
    if (group_id == 0)
        return nullptr;
    return map_at_ptr(get_data(gc).group_infos, group_id);
}

void update_groups_info(PurpleConnection* gc, vector<uint64> group_ids, const SuccessCb& success_cb)
{
    if (group_ids.empty()) {
        if (success_cb)
            success_cb();
        return;
    }

    string group_ids_str = str_concat_int(',', group_ids);
    vkcom_debug_info("Getting infos for groups %s\n", group_ids_str.data());

    CallParams params = { {"group_ids", str_concat_int(',', group_ids)} };
    vk_call_api(gc, "groups.getById", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }

        const picojson::array& groups = result.get<picojson::array>();
        for (const picojson::value& v: groups) {
            if (!field_is_present<double>(v, "id") || !field_is_present<string>(v, "name")
                    || !field_is_present<string>(v, "type")) {
                vkcom_debug_error("Wrong type returned as users.get call result: %s\n",
                                   result.serialize().data());
                return;
            }

            uint64 id = v.get("id").get<double>();
            VkGroupInfo& info = get_data(gc).group_infos[id];
            info.name = v.get("name").get<string>();
            info.type = v.get("type").get<string>();
            if (field_is_present<string>(v, "screen_name"))
                info.screen_name = v.get("screen_name").get<string>();
            info.last_updated = steady_clock::now();
        }

        if (success_cb)
            success_cb();
    }, [=](const picojson::value&) {
        if (success_cb)
            success_cb();
    });
}

string get_user_href(uint64 user_id, const VkUserInfo& info)
{
    if (!info.domain.empty())
        return str_format("<a href='https://vk.com/%s'>%s</a>", info.domain.data(),
                          info.real_name.data());
    else
        return str_format("<a href='https://vk.com/id%llu'>%s</a>", (unsigned long long)user_id,
                          info.real_name.data());
}

string get_group_href(uint64 group_id, const VkGroupInfo& info)
{
    if (!info.screen_name.empty())
        return str_format("<a href='https://vk.com/%s'>%s</a>", info.screen_name.data(), info.name.data());
    // How the fuck am I supposed to learn these URL patterns?
    if (info.type == "group") {
        return str_format("<a href='https://vk.com/club%llu'>%s</a>", (unsigned long long)group_id,
                          info.name.data());
    } else if (info.type == "page") {
        return str_format("<a href='https://vk.com/public%llu'>%s</a>",
                          (unsigned long long)group_id, info.name.data());
    } else if (info.type == "event") {
        return str_format("<a href='https://vk.com/event%llu'>%s</a>", (unsigned long long)group_id,
                          info.name.data());
    } else {
        vkcom_debug_error("Unknown group types %s\n", info.type.data());
        return "https://vk.com";
    }
}


PurpleConversation* find_conv_for_id(PurpleConnection* gc, uint64 user_id, uint64 chat_id)
{
    if (chat_id == 0)
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
