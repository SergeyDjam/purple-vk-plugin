#include "contutils.h"
#include "strutils.h"

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-chat.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-buddy.h"

namespace
{

// Parameters, we are requesting for users.
const char user_fields[] = "first_name,last_name,bdate,education,photo_50,photo_max_orig,"
                           "online,contacts,activity,last_seen,domain";

// Creates single string from multiple fields in user_fields, describing education.
string make_education_string(const picojson::value& v)
{
    string ret;
    if (field_is_present<string>(v, "university_name")) {
        ret = v.get("university_name").get<string>();
        if (ret.empty())
            return ret;
        if (field_is_present<string>(v, "faculty_name"))
            ret = v.get("faculty_name").get<string>() +  ", " + ret;
        if (field_is_present<double>(v, "graduation")) {
            int graduation = int(v.get("graduation").get<double>());
            if (graduation != 0) {
                ret += " ";

                char buf[128];
                // Strip '20' from graduation year
                if (graduation >= 2000)
                    sprintf(buf, "'%02d", graduation % 100);
                else
                    sprintf(buf, "%d", graduation);
                ret += buf;
            }
        }
    }
    return ret;
}

// Updates user info about user.
void update_user_info_from(PurpleConnection* gc, const picojson::value& fields)
{
    if (!field_is_present<double>(fields, "id")
            || !field_is_present<string>(fields, "first_name")
            || !field_is_present<string>(fields, "last_name")) {
        vkcom_debug_error("Incomplete user information in friends.get or users.get: %s\n",
                           picojson::value(fields).serialize().data());
        return;
    }
    uint64 user_id = fields.get("id").get<double>();

    VkUserInfo& info = get_data(gc).user_infos[user_id];
    info.real_name = fields.get("first_name").get<string>() + " " + fields.get("last_name").get<string>();

    // This usually means that user has been deleted.
    if (field_is_present<string>(fields, "deactivated"))
        return;

    if (field_is_present<string>(fields, "photo_50")) {
        info.photo_min = fields.get("photo_50").get<string>();
        static const char empty_photo_a[] = "http://vkontakte.ru/images/camera_a.gif";
        static const char empty_photo_b[] = "http://vkontakte.ru/images/camera_b.gif";
        static const char empty_photo_c[] = "https://vk.com/images/camera_c.gif";
        if (info.photo_min == empty_photo_a || info.photo_min == empty_photo_b
                || info.photo_min == empty_photo_c)
            info.photo_min.clear();
    }

    if (field_is_present<string>(fields, "activity"))
        info.activity = unescape_html(fields.get("activity").get<string>());
    else
        info.activity.clear();

    if (field_is_present<string>(fields, "bdate"))
        info.bdate = unescape_html(fields.get("bdate").get<string>());
    else
        info.bdate.clear();

    info.education = unescape_html(make_education_string(fields));

    if (field_is_present<string>(fields, "photo_max_orig"))
        info.photo_max = fields.get("photo_max_orig").get<string>();
    else
        info.photo_max.clear();

    if (field_is_present<string>(fields, "mobile_phone"))
        info.mobile_phone = unescape_html(fields.get("mobile_phone").get<string>());
    else
        info.mobile_phone.clear();

    if (field_is_present<string>(fields, "domain"))
        info.domain = fields.get("domain").get<string>();
    else
        info.domain.clear();
    if (info.domain == user_name_from_id(user_id))
        info.domain.clear();

    bool online = false;
    if (field_is_present<double>(fields, "online"))
        online = fields.get("online").get<double>() == 1;

    bool online_mobile = field_is_present<double>(fields, "online_mobile");

    // Update presence only for non-friends.
    if (!is_user_friend(gc, user_id)) {
        info.online = online;
        info.online_mobile = online_mobile;
    } else {
        if (info.online != online || info.online_mobile != online_mobile)
            vkcom_debug_error("Strange, got different online status for %llu"
                              " in friends.get vs Long Poll: %d, %d vs %d, %d\n",
                              (unsigned long long)user_id, online, online_mobile,
                              info.online, info.online_mobile);
    }

    if (field_is_present<picojson::object>(fields, "last_seen"))
        info.last_seen = fields.get("last_seen").get("time").get<double>();
}

// Returns all "id" elements from each item in items.
set<uint64> get_ids_from_items(const picojson::array& items)
{
    set<uint64> ret;
    for (const picojson::value& it: items) {
        if (!it.is<picojson::object>()) {
            vkcom_debug_error("Strange response: %s\n", it.serialize().data());
            return set<uint64>();
        }
        if (!field_is_present<double>(it, "id")) {
            vkcom_debug_error("Strange response: %s\n", it.serialize().data());
            return set<uint64>();
        }
        uint64 id = it.get("id").get<double>();
        ret.insert(id);
    }
    return ret;
}

// Updates friend_user_ids and information on friends (without presence information).
void update_friends_info(PurpleConnection* gc, const SuccessCb& success_cb)
{
    CallParams params = { {"user_id", to_string(get_data(gc).self_user_id())},
                          {"fields", user_fields} };
    vk_call_api(gc, "friends.get", params, [=](const picojson::value& result) {
        if (!result.is<picojson::object>()) {
            vkcom_debug_error("Strange response from friends.get: %s\n", result.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to update user infos"));
            return;
        }

        if (!result.get("items").is<picojson::array>()) {
            vkcom_debug_error("Strange response from friends.get: %s\n", result.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to update user infos"));
            return;
        }
        const picojson::array& items = result.get("items").get<picojson::array>();

        // We must update friend_user_ids before we update user infos, because we do not want
        // to update presence information.
        get_data(gc).friend_user_ids = get_ids_from_items(items);

        for (const picojson::value& v: items) {
            if (!v.is<picojson::object>()) {
                vkcom_debug_error("Strange response from friends.get: %s\n", v.serialize().data());
                continue;
            }

            update_user_info_from(gc, v);
        }

        success_cb();
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Unable to retrieve buddy list"));
    });
}

// We fill in members of this structure and then move them to corresponding VkData fields.
struct GetUsersChatsData
{
    set<uint64> user_ids;
    set<uint64> chat_ids;
};
typedef shared_ptr<GetUsersChatsData> GetUsersChatsData_ptr;

void get_users_chats_from_dialogs_impl(PurpleConnection* gc, const SuccessCb& success_cb,
                                       const GetUsersChatsData_ptr& data, size_t offset)
{
    CallParams params = { {"count", "200"},
                          {"offset", to_string(offset)},
                          {"preview_length", "1"} };
    vk_call_api(gc, "messages.getDialogs", params, [=](const picojson::value& v) {
        if (!field_is_present<double>(v, "count")
                || !field_is_present<picojson::array>(v, "items")) {
            vkcom_debug_error("Strange response from messages.getDialogs: %s\n",
                              v.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to retrieve dialogs list"));
            return;
        }

        VkData& gc_data = get_data(gc);

        uint64 count = v.get("count").get<double>();
        const picojson::array& items = v.get("items").get<picojson::array>();
        for (const picojson::value& m: items) {
            if (!field_is_present<picojson::object>(m, "message")) {
                vkcom_debug_error("Strange response from messages.getDialogs: %s\n",
                                  v.serialize().data());
                purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                               i18n("Unable to retrieve dialogs list"));
                return;
            }

            const picojson::value& message = m.get("message");
            if (field_is_present<double>(message, "chat_id")) {
                if (!field_is_present<string>(message, "title")
                        || !field_is_present<picojson::array>(message, "chat_active")
                        || !field_is_present<double>(message, "admin_id")) {
                    vkcom_debug_error("Strange response from our getDialogs: %s\n",
                                      v.serialize().data());
                    purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                                   i18n("Unable to retrieve dialogs list"));
                    return;
                }

                // If there are no chat participants, chat is inactive, ignore it (messages.getChat
                // returns an error in these cases).
                const picojson::array& chat_active = message.get("chat_active")
                                                            .get<picojson::array>();
                if (chat_active.size() == 0)
                    continue;

                // NOTE: we could parse chat title and participants and add entries to chat_infos,
                // but it's easier to do it via update_chat_infos.
                uint64 chat_id = message.get("chat_id").get<double>();
                data->chat_ids.insert(chat_id);
            } else {
                if (!field_is_present<double>(message, "user_id")) {
                    vkcom_debug_error("Strange response from messages.getDialogs: %s\n",
                                      v.serialize().data());
                    purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                                   i18n("Unable to retrieve dialogs list"));
                    return;
                }

                uint64 user_id = message.get("user_id").get<double>();
                data->user_ids.insert(user_id);
            }
        }

        size_t next_offset = offset + items.size();
        if (next_offset < count) {
            get_users_chats_from_dialogs_impl(gc, success_cb, data, next_offset);
        } else {
            gc_data.chat_ids = std::move(data->chat_ids);
            gc_data.dialog_user_ids = std::move(data->user_ids);
            success_cb();
        }
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Unable to retrieve dialogs list"));
    });
}

// Updates dialog_user_ids and chat_ids.
void get_users_chats_from_dialogs(PurpleConnection* gc, const SuccessCb& success_cb)
{
    GetUsersChatsData_ptr data{ new GetUsersChatsData() };
    get_users_chats_from_dialogs_impl(gc, success_cb, data, 0);
}

// Returns true if buddy with given user id should be shown in buddy list, false otherwise.
bool user_should_be_in_blist(PurpleConnection* gc, uint64 user_id)
{
    if (have_conversation_with_user(gc, user_id))
        return true;

    if (is_user_manually_removed(gc, user_id))
        return false;

    if (is_user_friend(gc, user_id))
        return true;

    if (is_user_manually_added(gc, user_id))
        return true;

    if (!get_data(gc).options().only_friends_in_blist && had_dialog_with_user(gc, user_id))
        return true;

    return false;
}

// Returns true if given chat should be shown in buddy list, false otherwise.
bool chat_should_be_in_blist(PurpleConnection* gc, uint64 chat_id)
{
    // Do we have chat open.
    if (chat_id_to_conv_id(gc, chat_id) != 0)
        return true;

    if (is_chat_manually_removed(gc, chat_id))
        return false;

    if (is_chat_manually_added(gc, chat_id))
        return true;

    if (get_data(gc).options().chats_in_blist && is_participant_in_chat(gc, chat_id))
        return true;
    else
        return false;
}

// Returns default group to add buddies to.
PurpleGroup* get_default_group(PurpleConnection* gc)
{
    const string& group_name = get_data(gc).options().blist_default_group;
    if (!group_name.empty())
        return purple_group_new(group_name.data());
    else
        return nullptr;
}

// Returns the default group to add chats to.
PurpleGroup* get_chat_group(PurpleConnection* gc)
{
    const string& group_name = get_data(gc).options().blist_chat_group;
    if (!group_name.empty())
        return purple_group_new(group_name.data());
    else
        return nullptr;
}

// Common code for checking if alias has changed for PurpleBuddy/PurpleChat.
void check_customized_alias(PurpleBlistNode* purple_node, VkBlistNode* node, const char* current_alias,
                            const string& default_alias)
{
    if (purple_blist_node_get_bool(purple_node, "custom-alias")) {
        // Check if node has been renamed back to default alias.
        if (default_alias == current_alias) {
            purple_blist_node_remove_setting(purple_node, "custom-alias");
            node->alias = current_alias;
        }
    } else {
        // Check if node has been renamed to different alias since last check. If node.alias
        // is empty, it has just been added to the buddy list.
        if (!node->alias.empty() && node->alias != current_alias) {
            vkcom_debug_info("Alias has been changed from %s to custom: %s\n", node->alias.data(), current_alias);

            purple_blist_node_set_bool(purple_node, "custom-alias", true);
            node->alias = current_alias;
        }
    }
}

// Common code for checking if group has changed for PurpleBuddy/PurpleChat.
void check_customized_group(PurpleBlistNode* purple_node, VkBlistNode* node, const char* current_group,
                            const string& default_group)
{
    if (purple_blist_node_get_bool(purple_node, "custom-group")) {
        // Check if node has been moved back to default group.
        if (default_group == current_group) {
            purple_blist_node_remove_setting(purple_node, "custom-group");
            node->group = current_group;
        }
    } else {
        // Check if node has been moved to another group since last check. If node.group
        // is empty, it has just been added to the buddy list.
        if (!node->group.empty() && node->group != current_group) {
            vkcom_debug_info("Group has been changed from %s to custom: %s\n", node->group.data(), current_group);

            purple_blist_node_set_bool(purple_node, "custom-group", true);
            node->group = current_group;
        }
    }
}

// Checks if buddy has been customized i.e. changed alias, group or has been removed since we checked last time.
void check_customized_buddy(PurpleConnection* gc, uint64 user_id, PurpleBuddy* buddy, VkBlistNode* node)
{
    VkData& gc_data = get_data(gc);
    if (!buddy) {
        gc_data.set_manually_removed_buddy(user_id);
        return;
    }

    VkUserInfo* info = get_user_info(gc, user_id);
    // We still haven't updated user info.
    if (!info)
        return;

    // Check if alias changed.
    const char* current_alias = purple_buddy_get_alias(buddy);
    if (!current_alias)
        current_alias = "";
    check_customized_alias(&buddy->node, node, current_alias, info->real_name);

    // Check if group changed.
    const char* current_group = purple_group_get_name(purple_buddy_get_group(buddy));
    check_customized_group(&buddy->node, node, current_group, gc_data.options().blist_default_group);
}

// Checks if chat has been customized i.e. changed alias, group or has been removed since we checked last time.
void check_customized_chat(PurpleConnection* gc, uint64 chat_id, PurpleChat* chat, VkBlistNode* node)
{
    VkData& gc_data = get_data(gc);
    if (!chat) {
        gc_data.set_manually_removed_chat(chat_id);
        return;
    }

    VkChatInfo* info = get_chat_info(gc, chat_id);
    // We still haven't updated chat info.
    if (!info)
        return;

    // Check if alias changed.
    const char* current_alias = purple_chat_get_name(chat);
    if (!current_alias)
        current_alias = "";
    check_customized_alias(&chat->node, node, current_alias, info->title);

    // Check if group changed.
    const char* current_group = purple_group_get_name(purple_chat_get_group(chat));
    check_customized_group(&chat->node, node, current_group, gc_data.options().blist_chat_group);
}

// Returns buddy status (in libpurple terms) from user info.
const char* get_user_status(const VkUserInfo& user_info)
{
    if (user_info.online_mobile) {
        return "mobile";
    } else if (user_info.online) {
        return "available";
    } else {
        return "offline";
    }
}

// Updates buddy presence status. It is a bit more complicated than simply calling purple_prpl_got_user_status,
// because we want to make sure buddy->icon is set. It is used e.g. in libnotify notifications
// "Buddy signed in". It is loaded lazily by Pidgin buddy list (this applies to all protocols,
// not only vkcom) and will not be loaded e.g. until buddy comes online (it will be loaded AFTER
// libnotify shows notification).
void update_buddy_presence_impl(PurpleConnection* gc, const string& buddy_name, const VkUserInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());
    if (!buddy)
        return;

    // Check if icon has not been already loaded. Icon must be loaded, otherwise pidgin-libnotify
    // will fail to show buddy icon.
    if (!purple_buddy_get_icon(buddy))
        // This method forces icons to be loaded.
        purple_buddy_icons_find(account, buddy_name.data());

    purple_prpl_got_user_status(account, buddy_name.data(), get_user_status(info), nullptr);
}

// We do not want to download more than one icon at once, so we have a queue. Fortunately, there
// is no need for locks, as we run everything from the main thread.
struct FetchBuddyIcon
{
    PurpleConnection* gc;
    string buddy_name;
    string icon_url;
};

vector<FetchBuddyIcon> fetch_queue;
// Number of currently running HTTP requests.
int fetches_running = 0;
// Maximum number of concurrently running HTTP requests
const int MAX_FETCHES_RUNNING = 4;

string get_filename(const char* url)
{
    string ret;
    str_rsplit(url, '/', nullptr, &ret);
    return ret;
}

void fetch_next_buddy_icon()
{
    FetchBuddyIcon fetch = fetch_queue.back();
    fetch_queue.pop_back();
    fetches_running++;
    vkcom_debug_info("Load buddy icon from %s\n", fetch.icon_url.data());
    http_get(fetch.gc, fetch.icon_url, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        vkcom_debug_info("Updating buddy icon for %s\n", fetch.buddy_name.data());
        if (!purple_http_response_is_successful(response)) {
            vkcom_debug_error("Error while fetching buddy icon: %s\n",
                               purple_http_response_get_error(response));
        } else {
            size_t icon_len;
            const void* icon_data = purple_http_response_get_data(response, &icon_len);
            const char* icon_url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
            // This should be synchronized with code in update_buddy_in_blist.
            string checksum = get_filename(icon_url);
            purple_buddy_icons_set_for_user(purple_connection_get_account(fetch.gc), fetch.buddy_name.data(),
                                            g_memdup(icon_data, icon_len), icon_len, checksum.data());
        }

        fetches_running--;
        if (!fetch_queue.empty())
            fetch_next_buddy_icon();
    });
}

// Starts downloading buddy icon and sets it upon finishing.
void fetch_buddy_icon(PurpleConnection* gc, const string& buddy_name, const string& icon_url)
{
    fetch_queue.push_back(FetchBuddyIcon{ gc, buddy_name, icon_url });
    if (fetches_running < MAX_FETCHES_RUNNING)
        fetch_next_buddy_icon();
}

// Adds or updates blist node for user_id.
void update_blist_buddy(PurpleConnection* gc, uint64 user_id, const VkUserInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    string buddy_name = user_name_from_id(user_id);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());

    // Check if the user has not modified buddy prior to modifying it ourselves.
    VkData& gc_data = get_data(gc);
    if (contains(gc_data.blist_buddies, user_id))
        check_customized_buddy(gc, user_id, buddy, &gc_data.blist_buddies[user_id]);

    PurpleGroup* group = get_default_group(gc);
    if (!buddy) {
        vkcom_debug_info("Adding %s to buddy list\n", buddy_name.data());
        buddy = purple_buddy_new(account, buddy_name.data(), nullptr);

        purple_blist_add_buddy(buddy, nullptr, group, nullptr);
        purple_blist_alias_buddy(buddy, info.real_name.data());
    } else {
        if (!purple_blist_node_get_bool(&buddy->node, "custom-alias")) {
            // Check if name has already been set, so that we do not get spurious "idXXXX is now known as ..."
            if (info.real_name != purple_buddy_get_alias(buddy)) {
                // Pidgin supports two types of aliases for buddies: "local"/"private" and "server". The local alias
                // is permanently stored in the buddy list and can be modified by the user (when the user modifies
                // some buddy alias, we set "custom-alias" for that node). The server alias is ephemeral and must be
                // set upon each login. Local status is considered dominant to server status. The only reason
                // why we call serv_got_alias is because that function conveniently writes "idXXXX is now known as YYY"
                // if conversation with that buddy is open.
                vkcom_debug_info("Renaming %s to %s\n", buddy_name.data(), info.real_name.data());
                purple_serv_got_private_alias(gc, buddy_name.data(), info.real_name.data());
            }
        }

        if (group && !purple_blist_node_get_bool(&buddy->node, "custom-group")) {
            // User set Group for Buddies. Check user has not set custom group for this particular buddy
            // and buddy is in the Group for Buddies.
            PurpleGroup* old_group = purple_buddy_get_group(buddy);
            if (!g_str_equal(purple_group_get_name(group), purple_group_get_name(old_group))) {
                vkcom_debug_info("Moving %s to %s\n", buddy_name.data(), purple_group_get_name(group));
                // add_buddy moves existing buddies from group to group.
                purple_blist_add_buddy(buddy, nullptr, group, nullptr);
            }
        }
    }

    // Store current alias/group information to check if it changed later.
    VkBlistNode& node = gc_data.blist_buddies[user_id];
    node.alias = purple_buddy_get_alias(buddy);
    node.group = purple_group_get_name(purple_buddy_get_group(buddy));

    update_buddy_presence_impl(gc, buddy_name, info);

    // Update last seen time.
    if (!info.online && !info.online_mobile) {
        if (info.last_seen != 0)
            // This is not documented, but set in libpurple, i.e. not Pidgin-specific.
            purple_blist_node_set_int(&buddy->node, "last_seen", info.last_seen);
        else
            vkcom_debug_error("Zero login time for %s\n", buddy_name.data());
    }

    // Either set empty avatar or add to download queue.
    if (info.photo_min.empty()) {
        purple_buddy_icons_set_for_user(account, buddy_name.data(), nullptr, 0, nullptr);
    } else {
        const char* checksum = purple_buddy_icons_get_checksum_for_user(buddy);
        // Icon url is a rather unstable checksum due to load balancing (the first part of the URL
        // can randomly change from one call to another, so we use only the last part, the filename,
        // which seems random enough to ignore potential collisions).
        if (!checksum || checksum != get_filename(info.photo_min.data()))
            fetch_buddy_icon(gc, buddy_name, info.photo_min);
    }
}

// Removes buddy from blist.
void remove_blist_buddy(PurpleConnection* gc, PurpleBuddy* buddy, uint64 user_id)
{
    vkcom_debug_info("Removing %s from buddy list\n", purple_buddy_get_name(buddy));
    get_data(gc).blist_buddies.erase(user_id);
    purple_blist_remove_buddy(buddy);
}

// Adds or updates blist node for chat_id.
void update_blist_chat(PurpleConnection* gc, uint64 chat_id, const VkChatInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    PurpleChat* chat = find_purple_chat_by_id(gc, chat_id);

    // Check if the user has not modified chat prior to modifying it ourselves.
    VkData& gc_data = get_data(gc);
    if (contains(gc_data.blist_chats, chat_id))
        check_customized_chat(gc, chat_id, chat, &gc_data.blist_chats[chat_id]);

    PurpleGroup* group = get_chat_group(gc);
    if (!chat) {
        string name = chat_name_from_id(chat_id);
        vkcom_debug_info("Adding %s to buddy list\n", name.data());

        GHashTable* components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(components, g_strdup("id"), g_strdup(name.data()));
        g_hash_table_insert(components, g_strdup("title"), g_strdup(info.title.data()));
        chat = purple_chat_new(account, info.title.data(), components);
        purple_blist_add_chat(chat, group, nullptr);
        purple_blist_alias_chat(chat, info.title.data());
    } else {
        if (!purple_blist_node_get_bool(&chat->node, "custom-alias")) {
            if (info.title != purple_chat_get_name(chat)) {
                vkcom_debug_info("Renaming chat%llu to %s\n", (unsigned long long)chat_id,
                                 info.title.data());
                purple_blist_alias_chat(chat, info.title.data());
            }

            GHashTable* components = purple_chat_get_components(chat);
            g_hash_table_insert(components, g_strdup("title"), g_strdup(info.title.data()));
        }

        if (group && !purple_blist_node_get_bool(&chat->node, "custom-group")) {
            // User set Group for Chats. Check user has not set custom group for this particular chat
            // and chat is in the Group for Chats.
            PurpleGroup* old_group = purple_chat_get_group(chat);
            if (!g_str_equal(purple_group_get_name(group), purple_group_get_name(old_group))) {
                vkcom_debug_info("Moving chat%llu to %s\n", (unsigned long long)chat_id,
                                 purple_group_get_name(group));
                // add_chat moves existing chats from group to group.
                purple_blist_add_chat(chat, group, nullptr);
            }
        }
    }

    // Store current alias/group information to check if it changed later.
    VkBlistNode& node = gc_data.blist_chats[chat_id];
    node.alias = purple_chat_get_name(chat);
    node.group = purple_group_get_name(purple_chat_get_group(chat));
}

// Removes chat from blist.
void remove_blist_chat(PurpleConnection* gc, PurpleChat* chat, uint64 chat_id)
{
    vkcom_debug_info("Removing chat%llu from buddy list\n", (unsigned long long)chat_id);
    get_data(gc).blist_chats.erase(chat_id);
    purple_blist_remove_chat(chat);
}

// Updates buddy list according to friends, user and chat infos. Adds new buddies, removes not required
// old buddies, updates buddy aliases and avatars. Buddy icons (avatars) are updated asynchronously.
void update_blist(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    VkData& gc_data = get_data(gc);

    // Check all currently known users if they should be added/updated to buddy list.
    for (const auto& p: gc_data.user_infos) {
        uint64 user_id = p.first;
        if (!user_should_be_in_blist(gc, user_id))
            continue;

        update_blist_buddy(gc, user_id, p.second);
    }

    // Check all current buddies in buddy list if they should be removed.
    GSList* buddies_list = purple_find_buddies(account, nullptr);
    for (GSList* it = buddies_list; it; it = it->next) {
        PurpleBuddy* buddy = (PurpleBuddy*)it->data;
        uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
        // Do not touch buddies which have been added by user.
        if (user_id == 0)
            continue;
        if (user_should_be_in_blist(gc, user_id))
            continue;

        remove_blist_buddy(gc, buddy, user_id);
    }
    g_slist_free(buddies_list);

    // Check all currently known chats if they should be added/updated to buddy list.
    for (const auto& p: gc_data.chat_infos) {
        uint64 chat_id = p.first;
        if (!chat_should_be_in_blist(gc, chat_id))
            continue;

        update_blist_chat(gc, chat_id, p.second);
    }

    // Check all current chats in buddyd list if they should be removed.
    for (PurpleChat* chat: find_all_purple_chats(account)) {
        const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
        if (!chat_name)
            continue;

        uint64 chat_id = chat_id_from_name(chat_name);
        if (chat_should_be_in_blist(gc, chat_id))
            continue;

        remove_blist_chat(gc, chat, chat_id);
    }
}

} // namespace

void update_user_chat_infos(PurpleConnection* gc)
{
    vkcom_debug_info("Updating full users and chats information\n");

    update_friends_info(gc, [=] {
        get_users_chats_from_dialogs(gc, [=]() {
            VkData& gc_data = get_data(gc);
            set<uint64> non_friend_user_ids;
            // Do not update user infos if we will not show users in blist anyway.
            if (!gc_data.options().only_friends_in_blist) {
                insert_if(non_friend_user_ids, gc_data.dialog_user_ids, [=](uint64 user_id) {
                    return !is_user_friend(gc, user_id);
                });
            }

            insert_if(non_friend_user_ids, gc_data.manually_added_buddies(), [=](uint64 user_id) {
                return !is_user_friend(gc, user_id);
            });

            update_user_infos(gc, non_friend_user_ids, [=] {
                update_chat_infos(gc, get_data(gc).chat_ids, [=] {
                    update_blist(gc);

                    // Chat titles, participants or buddy aliases could've changed.
                    update_all_open_chat_convs(gc);
                });
            });
        });
    });
}

void update_friends_presence(PurpleConnection* gc, const SuccessCb& on_update_cb)
{
    CallParams params = { {"online_mobile", "1"} };
    vk_call_api(gc, "friends.getOnline", params, [=](const picojson::value& result) {
        if (!field_is_present<picojson::array>(result, "online")
                || !field_is_present<picojson::array>(result, "online_mobile")) {
            vkcom_debug_error("Strange response from friends.getOnline: %s\n",
                              result.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to retrieve online info"));
            return;
        }

        VkData& gc_data = get_data(gc);
        set<uint64> friend_user_ids;

        const picojson::array& online = result.get("online").get<picojson::array>();
        for (const picojson::value& v: online) {
            if (!v.is<double>()) {
                vkcom_debug_error("Strange response from friends.getOnline: %s\n",
                                  result.serialize().data());
                purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                               i18n("Unable to retrieve online info"));
                return;
            }

            uint64 user_id = v.get<double>();
            friend_user_ids.insert(user_id);
            VkUserInfo& info = gc_data.user_infos[user_id];
            if (info.online && !info.online_mobile)
                continue;

            info.online = true;
            info.online_mobile = false;
            update_buddy_presence_impl(gc, user_name_from_id(user_id), info);
        }

        const picojson::array& online_mobile = result.get("online_mobile").get<picojson::array>();
        for (const picojson::value& v: online_mobile) {
            if (!v.is<double>()) {
                vkcom_debug_error("Strange response from friends.getOnline: %s\n",
                                  result.serialize().data());
                purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                               i18n("Unable to retrieve online info"));
                return;
            }

            uint64 user_id = v.get<double>();
            friend_user_ids.insert(user_id);
            VkUserInfo& info = gc_data.user_infos[user_id];
            if (info.online && info.online_mobile)
                continue;

            info.online = true;
            info.online_mobile = true;
            update_buddy_presence_impl(gc, user_name_from_id(user_id), info);
        }

        gc_data.friend_user_ids = std::move(friend_user_ids);

        if (on_update_cb)
            on_update_cb();
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Unable to retrieve online info"));
    });
}

void update_open_conv_presence(PurpleConnection *gc)
{
    vector<uint64> user_ids;
    VkData& gc_data = get_data(gc);
    for (const auto& it: gc_data.user_infos) {
        uint64 user_id = it.first;
        if (!is_user_friend(gc, user_id) && find_conv_for_id(gc, user_id, 0))
            user_ids.push_back(user_id);
    }

    if (user_ids.empty())
        return;

    string user_ids_str = str_concat_int(',', user_ids);
    vkcom_debug_info("Updating online status for buddies %s\n", user_ids_str.data());

    CallParams params = { {"fields", "online,online_mobile"} };
    vk_call_api_ids(gc, "users.get", params, "user_ids", user_ids, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Strange response from users.get: %s\n", result.serialize().data());
            return;
        }

        for (const picojson::value& v: result.get<picojson::array>()) {
            if (!v.is<picojson::object>()) {
                vkcom_debug_error("Strange response from users.get: %s\n",
                                   v.serialize().data());
                continue;
            }
            if (!field_is_present<double>(v, "id") || !field_is_present<double>(v, "online")) {
                vkcom_debug_error("Strange node found in users.get result: %s\n",
                                   v.serialize().data());
                continue;
            }
            uint64 user_id = v.get("id").get<double>();
            bool online = v.get("online").get<double>() == 1;
            bool online_mobile = field_is_present<double>(v, "online_mobile");
            vkcom_debug_info("Got status %d, %d for %llu\n", online, online_mobile,
                             (unsigned long long)user_id);

            VkUserInfo* info = get_user_info(gc, user_id);
            // We still have not updated info. It is highly unlikely, but still possible.
            if (!info)
                continue;
            if (info->online == online && info->online_mobile == online_mobile)
                continue;

            info->online = online;
            info->online_mobile = online_mobile;
            update_buddy_presence_impl(gc, user_name_from_id(user_id), *info);
        }
    }, nullptr, nullptr);
}

void update_user_infos(PurpleConnection* gc, const set<uint64>& user_ids, const SuccessCb& on_update_cb)
{
    if (user_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    string user_ids_str = str_concat_int(',', user_ids);
    vkcom_debug_info("Updating information on buddies %s\n", user_ids_str.data());

    CallParams params = { {"fields", user_fields} };
    vk_call_api_ids(gc, "users.get", params, "user_ids", to_vector(user_ids),
    [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Strange response from users.get: %s\n", result.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to update user infos"));
            return;
        }

        // Adds or updates buddies in result and forms the active set of buddy ids.
        for (const picojson::value& v: result.get<picojson::array>()) {
            if (!v.is<picojson::object>()) {
                vkcom_debug_error("Strange response from users.get: %s\n", v.serialize().data());
                continue;
            }
            update_user_info_from(gc, v);
        }
    }, [=] {
        if (on_update_cb)
            on_update_cb();
    }, [=](const picojson::value&) {
        // Do not disconnect as the error may be caused by the user being deleted (never seen it myself, but no
        // guarantees that it won't happen in the future).
        if (on_update_cb)
            on_update_cb();
    });
}

namespace
{

// Updates one entry in chat_infos. update_blist has the same meaning as in update_chat_infos
void update_chat_info_from(PurpleConnection* gc, const picojson::value& chat,
                           bool update_blist = false)
{
    if (!field_is_present<double>(chat, "id") || !field_is_present<string>(chat, "title")
            || !field_is_present<double>(chat, "admin_id")
            || !field_is_present<picojson::array>(chat, "users")) {
        vkcom_debug_error("Strange response from messages.getChat: %s\n", chat.serialize().data());
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Unable to retrieve chat info"));
        return;
    }

    uint64 chat_id = chat.get("id").get<double>();
    VkData& gc_data = get_data(gc);
    VkChatInfo& info = gc_data.chat_infos[chat_id];
    info.admin_id = chat.get("admin_id").get<double>();
    info.title = chat.get("title").get<string>();

    info.participants.clear();
    set<string> already_used_names;

    const picojson::array& users = chat.get("users").get<picojson::array>();
    for (const picojson::value& u: users) {
        if (!field_is_present<double>(u, "id")) {
            vkcom_debug_error("Strange response from messages.getChat: %s\n",
                              chat.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to retrieve chat info"));
            return;
        }

        // E-mail participants are less than zero, let's just ignore them. Also, ignore the user.
        int64 user_id = u.get("id").get<double>();
        if (user_id < 0 || gc_data.self_user_id() == (uint64)user_id)
            continue;

        // Do not update already known users.
        if (is_unknown_user(gc, user_id))
            update_user_info_from(gc, u);

        string user_name = get_user_display_name(gc, user_id);
        if (contains(already_used_names, user_name))
            user_name = get_unique_display_name(gc, user_id);
        already_used_names.insert(user_name);
        info.participants[user_id] = user_name;
    }

    // Adding self
    string self_name = get_self_chat_display_name(gc);
    uint64 self_user_id = get_data(gc).self_user_id();
    info.participants[self_user_id] = self_name;

    if (update_blist && chat_should_be_in_blist(gc, chat_id))
        update_blist_chat(gc, chat_id, info);

    int conv_id = chat_id_to_conv_id(gc, chat_id);
    if (conv_id != 0)
        update_open_chat_conv(gc, conv_id);
}

} // namespace

void update_chat_infos(PurpleConnection* gc, const set<uint64>& chat_ids,
                       const SuccessCb& on_update_cb, bool update_blist)
{
    if (chat_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    string chat_ids_str = str_concat_int(',', chat_ids);
    vkcom_debug_info("Updating information on chats %s\n", chat_ids_str.data());

    CallParams params = { {"fields", user_fields} };
    vk_call_api_ids(gc, "messages.getChat", params, "chat_ids", to_vector(chat_ids),
    [=](const picojson::value& v) {
        if (!v.is<picojson::array>()) {
            vkcom_debug_error("Strange response from messages.getChat: %s\n", v.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                           i18n("Unable to retrieve chat info"));
            return;
        }

        const picojson::array& a = v.get<picojson::array>();
        for (const picojson::value& chat: a)
            update_chat_info_from(gc, chat, update_blist);
    }, [=] {
        if (on_update_cb)
            on_update_cb();
    }, [=](const picojson::value&) {
        // Do not disconnect as the error may be caused by the chat being deactivated.
        if (on_update_cb)
            on_update_cb();
    });
}


void update_presence_in_blist(PurpleConnection *gc, uint64 user_id)
{
    VkUserInfo* info = get_user_info(gc, user_id);
    if (!info) {
        vkcom_debug_error("Programming error: update_presence_in_blist called without VkUserInfo set.\n");
        return;
    }

    update_buddy_presence_impl(gc, user_name_from_id(user_id), *info);
}


void add_buddies_if_needed(PurpleConnection* gc, const set<uint64>& user_ids, const SuccessCb& on_update_cb)
{
    if (user_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    // This function is called when receiving or sending the message to someone, so we should
    // add them to the list of dialog user ids.
    insert(get_data(gc).dialog_user_ids, user_ids);

    set<uint64> unknown_user_ids;
    insert_if(unknown_user_ids, user_ids, [=](uint64 user_id) {
        return is_unknown_user(gc, user_id);
    });

    update_user_infos(gc, unknown_user_ids, [=] {
        for (uint64 user_id: user_ids) {
            VkUserInfo* info = get_user_info(gc, user_id);
            if (info)
                update_blist_buddy(gc, user_id, *info);
        }
        if (on_update_cb)
            on_update_cb();
    });
}

void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb)
{
    if (user_in_buddy_list(gc, user_id) && !is_unknown_user(gc, user_id)) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    add_buddies_if_needed(gc, { user_id }, on_update_cb);
}

void remove_buddy_if_needed(PurpleConnection* gc, uint64 user_id)
{
    if (user_should_be_in_blist(gc, user_id))
        return;

    PurpleAccount* account = purple_connection_get_account(gc);
    string buddy_name = user_name_from_id(user_id);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());
    if (!buddy) {
        vkcom_debug_info("Trying to remove buddy %s not in buddy list\n",
                          buddy_name.data());
        return;
    }

    remove_blist_buddy(gc, buddy, user_id);
}

void add_chats_if_needed(PurpleConnection* gc, const set<uint64>& chat_ids, const SuccessCb& on_update_cb)
{
    if (chat_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    // This function is called when receiving or sending the message to some chat, so we should
    // add it to the list of chat ids.
    insert(get_data(gc).chat_ids, chat_ids);

    set<uint64> unknown_chat_ids;
    insert_if(unknown_chat_ids, chat_ids, [=](uint64 chat_id) {
        return is_unknown_chat(gc, chat_id);
    });

    update_chat_infos(gc, unknown_chat_ids, [=] {
        for (uint64 chat_id: chat_ids) {
            VkChatInfo* info = get_chat_info(gc, chat_id);
            if (info)
                update_blist_chat(gc, chat_id, *info);
        }
        if (on_update_cb)
            on_update_cb();
    });
}

void add_chat_if_needed(PurpleConnection* gc, uint64 chat_id, const SuccessCb& on_update_cb)
{
    if (chat_in_buddy_list(gc, chat_id) && !is_unknown_chat(gc, chat_id)) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    add_chats_if_needed(gc, { chat_id }, on_update_cb);
}

void remove_chat_if_needed(PurpleConnection* gc, uint64 chat_id)
{
    if (chat_should_be_in_blist(gc, chat_id))
        return;

    PurpleChat* chat = find_purple_chat_by_id(gc, chat_id);
    if (!chat) {
        vkcom_debug_info("Trying to remove chat%llu not in buddy list\n",
                         (unsigned long long)chat_id);
        return;
    }

    remove_blist_chat(gc, chat, chat_id);
}


void check_blist_on_login(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    VkData& data = get_data(gc);
    for (PurpleBlistNode* node = purple_blist_get_root(); node; node = purple_blist_node_next(node, FALSE)) {
        if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
            PurpleBuddy* buddy = PURPLE_BUDDY(node);
            if (purple_buddy_get_account(buddy) != account)
                continue;

            uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
            if (user_id == 0)
                continue;

            VkBlistNode& vk_node = data.blist_buddies[user_id];
            vk_node.alias = purple_buddy_get_alias(buddy);
            vk_node.group = purple_group_get_name(purple_buddy_get_group(buddy));
        } else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
            PurpleChat* chat = PURPLE_CHAT(node);
            if (purple_chat_get_account(chat) != account)
                continue;

            const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
            // Do nothing for chats which have been added by user.
            if (!chat_name)
                return;

            uint64 chat_id = chat_id_from_name(chat_name);

            VkBlistNode& vk_node = data.blist_chats[chat_id];
            vk_node.alias = purple_chat_get_name(chat);
            vk_node.group = purple_group_get_name(purple_chat_get_group(chat));
        }
    }
}

void check_blist_on_logout(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    map<uint64, PurpleBuddy*> buddies;
    GSList* buddies_list = purple_find_buddies(account, nullptr);
    for (GSList* it = buddies_list; it; it = it->next) {
        PurpleBuddy* buddy = (PurpleBuddy*)it->data;
        uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
        if (user_id == 0)
            continue;

        buddies[user_id] = buddy;
    }
    g_slist_free(buddies_list);

    VkData& gc_data = get_data(gc);
    for (auto& p: gc_data.blist_buddies) {
        uint64 user_id = p.first;
        PurpleBuddy* buddy = map_at_default(buddies, user_id, nullptr);
        VkBlistNode* node = &p.second;
        check_customized_buddy(gc, user_id, buddy, node);
    }

    map<uint64, PurpleChat*> chats;
    for (PurpleChat* chat: find_all_purple_chats(account)) {
        const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
        // Do nothing for chats which have been added by user.
        if (!chat_name)
            continue;

        uint64 chat_id = chat_id_from_name(chat_name);
        if (chat_id == 0)
            continue;

        chats[chat_id] = chat;
    }

    for (auto& p: gc_data.blist_chats) {
        uint64 chat_id = p.first;
        PurpleChat* chat = map_at_default(chats, chat_id, nullptr);
        VkBlistNode* node = &p.second;
        check_customized_chat(gc, chat_id, chat, node);
    }
}
