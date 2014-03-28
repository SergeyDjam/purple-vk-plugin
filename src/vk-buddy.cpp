#include <debug.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-chat.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-buddy.h"

namespace
{

// Helper callback, used for friends.get and users.get. Adds and/or updates all buddies info in
// VkConnData->user_infos from the result. Returns list of user ids.
//
// friends_get must be true if the function is called for friends.get result, false for users.get
// (these two methods have actually slightly different response objects);
// update_presence has the same meaning as in update_buddy_list
uint64_set update_user_infos(PurpleConnection* gc, const picojson::value& result, bool friends_get,
                                bool update_presence);

// Updates VkConnData::dialog_user_ids, ::chat_ids and ::chat_infos for chats user participates in.
void get_users_chats_from_dialogs(PurpleConnection* gc, const SuccessCb& success_cb);

// Updates buddy list according to friend_user_ids and user_infos stored in VkConnData. Adds new buddies, removes
// old buddies, updates buddy aliases and avatars. Buddy icons (avatars) are updated asynchronously.
void update_buddy_list(PurpleConnection* gc, bool update_presence);

} // End of anonymous namespace

// Parameters, we are requesting for users.
const char user_fields[] = "first_name,last_name,bdate,education,photo_50,photo_max_orig,"
                           "online,contacts,activity,last_seen,domain";

void update_buddies(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb)
{
    vkcom_debug_info("Updating full buddy list\n");

    VkConnData* conn_data = get_conn_data(gc);
    CallParams params = { {"user_id", to_string(conn_data->self_user_id())}, {"fields", user_fields} };
    vk_call_api(gc, "friends.get", params, [=](const picojson::value& result) {
        conn_data->friend_user_ids = update_user_infos(gc, result, true, update_presence);
        get_users_chats_from_dialogs(gc, [=]() {
            uint64_set non_friend_user_ids;
            if (!conn_data->options.only_friends_in_blist) {
                append_if(non_friend_user_ids, conn_data->dialog_user_ids, [=](uint64 user_id) {
                    return !user_is_friend(gc, user_id);
                });
            }

            // We could've manually added buddy and he has become our friend later.
            append_if(non_friend_user_ids, conn_data->manually_added_buddies, [=](uint64 user_id) {
                return !user_is_friend(gc, user_id);
            });

            // Add all chat participants. We do not really need that much information about them,
            // so in the future we could've asked only for the names.
            for (const pair<uint64, VkChatInfo>& c: conn_data->chat_infos) {
                append_if(non_friend_user_ids, c.second.participants, [=] (uint64 user_id) {
                    return !user_is_friend(gc, user_id);
                });
            }

            uint64_vec non_friend_user_ids_vec;
            assign(non_friend_user_ids_vec, non_friend_user_ids);
            add_or_update_user_infos(gc, non_friend_user_ids_vec, [=] {
                update_buddy_list(gc, update_presence);

                // Chat titles, participants or buddy aliases could've changed.
                update_all_open_chat_convs(gc);

                if (on_update_cb)
                    on_update_cb();
            });
        });
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve buddy list");
    });
}

namespace
{

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

// Updates user info about buddy and returns buddy user id or zero in case of failure.
uint64 update_user_info(PurpleConnection* gc, const picojson::value& fields, bool update_presence)
{
    if (!field_is_present<double>(fields, "id")
            || !field_is_present<string>(fields, "first_name")
            || !field_is_present<string>(fields, "last_name")) {
        vkcom_debug_error("Incomplete user information in friends.get or users.get: %s\n",
                           picojson::value(fields).serialize().data());
        return 0;
    }
    uint64 user_id = fields.get("id").get<double>();

    VkConnData* conn_data = get_conn_data(gc);
    VkUserInfo& info = conn_data->user_infos[user_id];
    info.real_name = fields.get("first_name").get<string>() + " " + fields.get("last_name").get<string>();

    // This usually means that user has been deleted.
    if (field_is_present<string>(fields, "deactivated"))
        return 0;

    if (field_is_present<string>(fields, "photo_50")) {
        info.photo_min = fields.get("photo_50").get<string>();
        static const char empty_photo_a[] = "http://vkontakte.ru/images/camera_a.gif";
        static const char empty_photo_b[] = "http://vkontakte.ru/images/camera_b.gif";
        static const char empty_photo_c[] = "http://vk.com/images/camera_c.gif";
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

    if (update_presence) {
        info.online = online;
        info.online_mobile = online_mobile;
    } else {
        if (info.online != online || info.online_mobile != online_mobile)
            vkcom_debug_error("Strange, got different online status for %" PRIu64
                               " in friends.get vs Long Poll: %d, %d vs %d, %d\n",
                               user_id, online, online_mobile, info.online, info.online_mobile);
    }

    if (field_is_present<picojson::object>(fields, "last_seen"))
        info.last_seen = fields.get("last_seen").get("time").get<double>();

    return user_id;
}

uint64_set update_user_infos(PurpleConnection* gc, const picojson::value& result, bool friends_get, bool update_presence)
{
    if (friends_get && !result.is<picojson::object>()) {
        vkcom_debug_error("Wrong type returned as friends.get call result\n");
    /*
        return {};
        As I can understand, it is all right here, but clang 3.2 doesn't compile it.
        http://clang-developers.42468.n3.nabble.com/C-11-error-about-initializing-explicit-constructor-with-td4029849.html
    */
        return uint64_set();
    }

    const picojson::value& items = friends_get ? result.get("items") : result;
    if (!items.is<picojson::array>()) {
        vkcom_debug_error("Wrong type returned as friends.get or users.get call result\n");
        return uint64_set();
    }

    // Adds or updates buddies in result and forms the active set of buddy ids.
    uint64_set buddy_user_ids;
    for (const picojson::value& v: items.get<picojson::array>()) {
        if (!v.is<picojson::object>()) {
            vkcom_debug_error("Strange node found in friends.get or users.get result: %s\n",
                               v.serialize().data());
            continue;
        }
        uint64 user_id = update_user_info(gc, v, update_presence);
        if (user_id != 0)
            buddy_user_ids.insert(user_id);
    }

    return buddy_user_ids;
}

// Returns true if buddy with given user id should be shown in buddy list, false otherwise.
bool user_should_be_in_blist(PurpleConnection* gc, uint64 user_id)
{
    if (have_conversation_with_user(gc, user_id))
        return true;

    if (is_user_manually_removed(gc, user_id))
        return false;

    if (user_is_friend(gc, user_id))
        return true;

    if (is_user_manually_added(gc, user_id))
        return true;

    VkConnData* conn_data = get_conn_data(gc);
    if (!conn_data->options.only_friends_in_blist && had_dialog_with_user(gc, user_id))
        return true;

    return false;
}

// Returns true if given chat should be shown in buddy list, false otherwise.
bool chat_should_be_in_blist(PurpleConnection* gc, uint64 chat_id)
{
    // Do we have chat open.
    if (chat_id_to_conv_id(gc, chat_id) != 0)
        return true;

    if (is_chat_manually_added(gc, chat_id))
        return true;

    VkConnData* conn_data = get_conn_data(gc);
    if (conn_data->options.chats_in_blist && participant_in_chat(gc, chat_id))
        return true;
    else
        return false;
}

// Returns default group to add buddies to.
PurpleGroup* get_default_group(PurpleConnection* gc)
{
    VkConnData* conn_data = get_conn_data(gc);
    const string& group_name = conn_data->options.blist_default_group;
    if (!group_name.empty())
        return purple_group_new(group_name.data());
    else
        return nullptr;
}

// Returns the default group to add chats to.
PurpleGroup* get_chat_group(PurpleConnection* gc)
{
    VkConnData* conn_data = get_conn_data(gc);
    const string& group_name = conn_data->options.blist_chat_group;
    if (!group_name.empty())
        return purple_group_new(group_name.data());
    else
        return nullptr;
}


// Starts downloading buddy icon and sets it upon finishing.
void fetch_buddy_icon(PurpleConnection* gc, const string& buddy_name, const string& icon_url);

// Checks if buddy alias has been changed, updates VkConnData::buddy_blist_last_alias and "custom-alias" tag.
void check_buddy_alias(PurpleConnection* gc, PurpleBuddy* buddy)
{
    uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
    // Do nothing for buddies which have been added by user.
    if (user_id == 0)
        return;

    VkConnData* conn_data = get_conn_data(gc);
    VkUserInfo* info = get_user_info(gc, user_id);
    // We still haven't updated user info.
    if (!info)
        return;

    const char* current_alias = purple_buddy_get_alias(buddy);

    if (purple_blist_node_get_bool(&buddy->node, "custom-alias")) {
        // Check if buddy has been renamed back to its name.
        if (current_alias == info->real_name)
            purple_blist_node_remove_setting(&buddy->node, "custom-alias");
    } else {
        // Check if buddy has been renamed since last check.
        if (contains(conn_data->buddy_blist_last_alias, user_id)
                && conn_data->buddy_blist_last_alias[user_id] != current_alias) {
            vkcom_debug_info("Buddy alias for %s has been changed to custom: %s\n",
                              purple_buddy_get_name(buddy), current_alias);

            purple_blist_node_set_bool(&buddy->node, "custom-alias", true);
        } else {
            // Buddy either has not been renamed or has just been added. Check if buddy name has been updated.
            if (current_alias != info->real_name) {
                // Pidgin supports two types of aliases for buddies: "local"/"private" and "server". The local alias
                // is permanently stored in the buddy list and can be modified by the user (when the user modifies
                // some buddy alias, we set "custom-alias" for that node). The server alias is ephemeral and must be
                // set upon each login. Local status is considered dominant to server status. The only reason
                // why we call serv_got_alias is because that function conveniently writes "idXXXX is now known as YYY"
                // if conversation with that buddy is open. Otherwise, server alias is completely ignored.
                purple_serv_got_private_alias(gc, purple_buddy_get_name(buddy), info->real_name.data());
            }
        }
    }

    conn_data->buddy_blist_last_alias[user_id] = current_alias;
}

// Checks if chat alias has been changed, updates VkConnData::chat_blist_last_alias and "custom-alias" tag.
void check_chat_alias(PurpleConnection* gc, PurpleChat* chat)
{
    const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
    // Do nothing for chats which have been added by user.
    if (!chat_name)
        return;

    VkConnData* conn_data = get_conn_data(gc);
    uint64 chat_id = chat_id_from_name(chat_name);
    VkChatInfo& info = conn_data->chat_infos[chat_id];
    const char* current_alias = chat->alias;

    if (purple_blist_node_get_bool(&chat->node, "custom-alias")) {
        // Check if chat alias has been set back to its title.
        if (current_alias == info.title)
            purple_blist_node_remove_setting(&chat->node, "custom-alias");
    } else {
        // Check if chat has been renamed since last check.
        if (contains(conn_data->chat_blist_last_alias, chat_id)
                && conn_data->chat_blist_last_alias[chat_id] != current_alias) {
            vkcom_debug_info("Chat alias for %s has been changed to custom: %s\n",
                              chat_name, current_alias);

            purple_blist_node_set_bool(&chat->node, "custom-alias", true);
        } else {
            // Chat either has not been renamed or has just been added. Check if chat title has been updated.
            if (current_alias != info.title)
                purple_blist_alias_chat(chat, info.title.data());
            current_alias = info.title.data();
        }
    }

    conn_data->chat_blist_last_alias[chat_id] = current_alias;
}

// Checks if buddy was moved from group to group, updates VkConnData::buddy_blist_last_group and "custom-group" tag.
void check_buddy_group(PurpleConnection* gc, PurpleBuddy* buddy)
{
    uint64 user_id = user_id_from_name(purple_buddy_get_name(buddy));
    // Do nothing for buddies which have been added by user.
    if (user_id == 0)
        return;

    VkConnData* conn_data = get_conn_data(gc);
    const char* current_group = purple_group_get_name(purple_buddy_get_group(buddy));

    if (purple_blist_node_get_bool(&buddy->node, "custom-group")) {
        // Check if buddy has been moved back to default group.
        if (conn_data->options.blist_default_group == current_group)
            purple_blist_node_remove_setting(&buddy->node, "custom-group");
    } else {
        // Check if buddy has been moved to another group since last check.
        if (contains(conn_data->buddy_blist_last_group, user_id)
                && conn_data->buddy_blist_last_group[user_id] != current_group) {
            vkcom_debug_info("Buddy group for %s has been changed to custom: %s\n",
                              purple_buddy_get_name(buddy), current_group);

            purple_blist_node_set_bool(&buddy->node, "custom-group", true);
        } else {
            // Check if default group has changed and move to new default group if needed.
            if (!conn_data->options.blist_default_group.empty()
                    && conn_data->options.blist_default_group != current_group) {
                purple_blist_add_buddy(buddy, nullptr, get_default_group(gc), nullptr);
                current_group = conn_data->options.blist_default_group.data();
            }
        }
    }

    conn_data->buddy_blist_last_group[user_id] = current_group;
}

// Checks if was moved from group to group, updates VkConnData::chat_blist_last_group and "custom-group" tag.
void check_chat_group(PurpleConnection* gc, PurpleChat* chat)
{
    const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
    // Do nothing for chats which have been added by user.
    if (!chat_name)
        return;

    VkConnData* conn_data = get_conn_data(gc);
    uint64 chat_id = chat_id_from_name(chat_name);
    const char* current_group = purple_group_get_name(purple_chat_get_group(chat));

    if (purple_blist_node_get_bool(&chat->node, "custom-group")) {
        // Check if chat has been moved back to default group.
        if (conn_data->options.blist_chat_group == current_group)
            purple_blist_node_remove_setting(&chat->node, "custom-group");
    } else {
        // Check if chat has been moved to another group since last check.
        if (contains(conn_data->chat_blist_last_group, chat_id)
                && conn_data->chat_blist_last_group[chat_id] != current_group) {
            vkcom_debug_info("Buddy group for %s has been changed to custom: %s\n", chat_name,
                              current_group);

            purple_blist_node_set_bool(&chat->node, "custom-group", true);
        } else {
            // Check if default group has changed and move to new default group if needed.
            if (!conn_data->options.blist_chat_group.empty()
                    && conn_data->options.blist_chat_group != current_group) {
                purple_blist_add_chat(chat, get_default_group(gc), nullptr);
                current_group = conn_data->options.blist_chat_group.data();
            }
        }
    }

    conn_data->chat_blist_last_group[chat_id] = current_group;
}

// Returns buddy status (in libpurple terms) from user info.
const char* get_user_status(const VkUserInfo& user_info)
{
    if (user_info.online_mobile) {
        return "mobile";
    } else if (user_info.online) {
        return "online";
    } else {
        return "offline";
    }
}

// Updates buddy presence status. It is a bit more complicated than simply calling purple_prpl_got_user_status,
// because we want to make sure buddy->icon is set. It is used e.g. in libnotify notifications
// "Buddy signed in". It is loaded lazily by Pidgin buddy list (this applies to all protocols,
// not only vkcom) and will not be loaded e.g. until buddy comes online (it will be loaded AFTER
// libnotify shows notification).
void update_buddy_presence_internal(PurpleConnection* gc, const string& buddy_name, const VkUserInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());

    // Check if icon has not been already loaded. Icon must be loaded, otherwise pidgin-libnotify
    // will fail to show buddy icon.
    if (!purple_buddy_get_icon(buddy))
        // This method forces icons to be loaded.
        purple_buddy_icons_find(account, buddy_name.data());

    purple_prpl_got_user_status(account, buddy_name.data(), get_user_status(info), nullptr);
}

void update_buddy_in_blist(PurpleConnection* gc, uint64 user_id, const VkUserInfo& info, bool update_presence)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    string buddy_name = user_name_from_id(user_id);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());

    if (!buddy) {
        vkcom_debug_info("Adding %s to buddy list\n", buddy_name.data());
        buddy = purple_buddy_new(account, buddy_name.data(), nullptr);

        PurpleGroup* group = get_default_group(gc);
        purple_blist_add_buddy(buddy, nullptr, group, nullptr);
    }

    check_buddy_alias(gc, buddy);
    check_buddy_group(gc, buddy);

    // Update presence
    if (update_presence) {
        update_buddy_presence_internal(gc, buddy_name, info);
    } else {
        // We do not update online/offline status here, because it is done in Long Poll processing but we
        // "update" it so that status strings in buddy list get updated (vk_status_text gets called).
        PurpleStatus* status = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
        purple_prpl_got_user_status(account, buddy_name.data(), purple_status_get_id(status), nullptr);
    }

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
        if (!checksum || checksum != str_rsplit(info.photo_min, '/'))
            fetch_buddy_icon(gc, buddy_name, info.photo_min);
    }
}

// Returns the list of all chats corresponding to this account in buddy list.

void update_chat_in_blist(PurpleConnection* gc, uint64 chat_id, const VkChatInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    PurpleChat* chat = find_purple_chat_by_id(gc, chat_id);
    PurpleGroup* group = get_chat_group(gc);

    if (!chat) {
        string name = chat_name_from_id(chat_id);
        vkcom_debug_info("Adding %s to buddy list\n", name.data());

        GHashTable* components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(components, g_strdup("id"), g_strdup(name.data()));
        chat = purple_chat_new(account, info.title.data(), components);
        purple_blist_add_chat(chat, group, nullptr);
    }

    check_chat_alias(gc, chat);
    check_chat_group(gc, chat);
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
            string checksum = str_rsplit(icon_url, '/');
            purple_buddy_icons_set_for_user(purple_connection_get_account(fetch.gc), fetch.buddy_name.data(),
                                            g_memdup(icon_data, icon_len), icon_len, checksum.data());
        }

        fetches_running--;
        if (!fetch_queue.empty())
            fetch_next_buddy_icon();
    });
}

void fetch_buddy_icon(PurpleConnection* gc, const string& buddy_name, const string& icon_url)
{
    fetch_queue.push_back(FetchBuddyIcon({ gc, buddy_name, icon_url }));
    if (fetches_running < MAX_FETCHES_RUNNING)
        fetch_next_buddy_icon();
}

// Checks if user ids are present in buddy list and adds them if they are not. Ignores "friends only in buddy list"
// setting.
void update_buddy_list_for_users(PurpleConnection* gc, const uint64_vec& user_ids, bool update_presence)
{
    for (uint64 user_id: user_ids) {
        VkUserInfo* info = get_user_info(gc, user_id);
        if (info)
            update_buddy_in_blist(gc, user_id, *info, update_presence);
    }
}

} // anonymous namespace

void add_buddies_if_needed(PurpleConnection* gc, const uint64_vec& user_ids, const SuccessCb& on_update_cb)
{
    if (user_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    // This function is called when receiving or sending the message to someone, so we should
    // add them to the list of dialog user ids.
    VkConnData* conn_data = get_conn_data(gc);
    append(conn_data->dialog_user_ids, user_ids);

    uint64_vec unknown_user_ids;
    append_if(unknown_user_ids, user_ids, [=](uint64 user_id) {
        return is_unknown_user(gc, user_id);
    });

    add_or_update_user_infos(gc, unknown_user_ids, [=] {
        update_buddy_list_for_users(gc, user_ids, true);
        if (on_update_cb)
            on_update_cb();
    });
}

void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb)
{
    if (user_in_buddy_list(gc, user_id)) {
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

    vkcom_debug_info("Removing %s from buddy list as unneeded after convo close\n",
                      buddy_name.data());
    purple_blist_remove_buddy(buddy);
}

void add_or_update_user_infos(PurpleConnection* gc, const uint64_vec& user_ids, const SuccessCb& on_update_cb)
{
    if (user_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    for (uint64 user_id: user_ids) {
        assert(!user_is_friend(gc, user_id));
    }

    string user_ids_str = str_concat_int(',', user_ids);
    vkcom_debug_info("Updating information for buddies %s\n", user_ids_str.data());

    CallParams params = { {"fields", user_fields} };
    vk_call_api_ids(gc, "users.get", params, "user_ids", user_ids, [=](const picojson::value& result) {
        update_user_infos(gc, result, false, true);
    }, [=] {
        if (on_update_cb)
            on_update_cb();
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve user info");
    });
}


void add_chats_if_needed(PurpleConnection* gc, const uint64_vec& chat_ids, const SuccessCb& on_update_cb)
{
    if (chat_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    // This function is called when receiving or sending the message to some chat, so we should
    // add it to the list of chat ids.
    VkConnData* conn_data = get_conn_data(gc);
    append(conn_data->chat_ids, chat_ids);

    uint64_vec unknown_chat_ids;
    append_if(unknown_chat_ids, chat_ids, [=](uint64 chat_id) {
        return is_unknown_chat(gc, chat_id);
    });

    add_or_update_chat_infos(gc, unknown_chat_ids, [=] {
        for (uint64 chat_id: chat_ids) {
            VkChatInfo* info = get_chat_info(gc, chat_id);
            if (info)
                update_chat_in_blist(gc, chat_id, *info);
        }
        if (on_update_cb)
            on_update_cb();
    });
}

void add_chat_if_needed(PurpleConnection* gc, uint64 chat_id, const SuccessCb& on_update_cb)
{
    if (chat_in_buddy_list(gc, chat_id)) {
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
        vkcom_debug_info("Trying to remove chat%" PRIu64 " not in buddy list\n", chat_id);
        return;
    }

    vkcom_debug_info("Removing chat%" PRIu64 " from buddy list as unneeded after convo close\n", chat_id);
    purple_blist_remove_chat(chat);
}

// Parameters, we are requesting for chat participants.
const char chat_user_fields[] = "first_name,last_name";

// Updates one entry in VkConnData::chat_infos.
void update_chat_info(PurpleConnection* gc, const picojson::value& chat)
{
    if (!field_is_present<double>(chat, "id") || !field_is_present<string>(chat, "title")
            || !field_is_present<double>(chat, "admin_id") || !field_is_present<picojson::array>(chat, "users")) {
        vkcom_debug_error("Strange response from messages.getChat: %s\n", chat.serialize().data());
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve chat info");
        return;
    }

    uint64 chat_id = chat.get("id").get<double>();
    VkConnData* conn_data = get_conn_data(gc);
    VkChatInfo& info = conn_data->chat_infos[chat_id];
    info.admin_id = chat.get("admin_id").get<double>();
    info.title = chat.get("title").get<string>();
    info.participants.clear();
    const picojson::array& users = chat.get("users").get<picojson::array>();
    for (const picojson::value& u: users) {
        if (!field_is_present<double>(u, "id")) {
            vkcom_debug_error("Strange response from messages.getChat: %s\n", chat.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve chat info");
            return;
        }

        // E-mail participants are less than zero, let's just ignore them. Also, ignore the user.
        int64 user_id = u.get("id").get<double>();
        if (user_id < 0 || conn_data->self_user_id() == (uint64)user_id)
            continue;
        info.participants.push_back(user_id);

        // Do not update already known users.
        if (is_unknown_user(gc, user_id))
            update_user_info(gc, u, false);
    }

    int conv_id = chat_id_to_conv_id(gc, chat_id);
    if (conv_id != 0)
        update_open_chat_conv(gc, conv_id);
}

void add_or_update_chat_infos(PurpleConnection* gc, const uint64_vec& chat_ids, const SuccessCb& on_update_cb)
{
    if (chat_ids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    string chat_ids_str = str_concat_int(',', chat_ids);
    vkcom_debug_info("Updating information for chats %s\n", chat_ids_str.data());

    CallParams params = { {"fields", chat_user_fields} };
    vk_call_api_ids(gc, "messages.getChat", params, "chat_ids", chat_ids, [=](const picojson::value& v) {
        if (!v.is<picojson::array>()) {
            vkcom_debug_error("Strange response from messages.getChat: %s\n", v.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve chat info");
            return;
        }

        const picojson::array& a = v.get<picojson::array>();
        for (const picojson::value& chat: a)
            update_chat_info(gc, chat);
    }, [=] {
        if (on_update_cb)
            on_update_cb();
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve chat info");
    });
}

namespace
{

// We fill in members of this structure and then move them to corresponding VkConnData fields.
struct GetUsersChatsData
{
    uint64_set user_ids;
    uint64_set chat_ids;
};
typedef shared_ptr<GetUsersChatsData> GetUsersChatsData_ptr;

void get_users_chats_from_dialogs_impl(PurpleConnection* gc, const SuccessCb& success_cb,
                                       const GetUsersChatsData_ptr& data, uint offset)
{
    CallParams params = { {"count", "200"}, {"offset", to_string(offset)} };
    vk_call_api(gc, "messages.getDialogs", params, [=](const picojson::value& v) {
        if (!field_is_present<double>(v, "count") || !field_is_present<picojson::array>(v, "items")) {
            vkcom_debug_error("Strange response from messages.getDialogs: %s\n", v.serialize().data());
            purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
            return;
        }

        VkConnData* conn_data = get_conn_data(gc);

        uint64 count = v.get("count").get<double>();
        const picojson::array& items = v.get("items").get<picojson::array>();
        for (const picojson::value& m: items) {
            if (!field_is_present<picojson::object>(m, "message")) {
                vkcom_debug_error("Strange response from messages.getDialogs: %s\n", v.serialize().data());
                purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
                return;
            }

            const picojson::value& message = m.get("message");
            if (field_is_present<double>(message, "chat_id")) {
                if (!field_is_present<string>(message, "title")
                        || !field_is_present<picojson::array>(message, "chat_active")
                        || !field_is_present<double>(message, "admin_id")) {
                    vkcom_debug_error("Strange response from our getDialogs: %s\n", v.serialize().data());
                    purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
                    return;
                }

                // If there are no chat participants, either chat is inactive, ignore it (messages.getChat
                // returns an error in these cases).
                const picojson::array& chat_active = message.get("chat_active").get<picojson::array>();
                if (chat_active.size() == 0)
                    continue;

                uint64 chat_id = message.get("chat_id").get<double>();
                data->chat_ids.insert(chat_id);

                VkChatInfo& info = conn_data->chat_infos[chat_id];
                info.admin_id = message.get("admin_id").get<double>();
                info.title = message.get("title").get<string>();
                info.participants.clear();
                for (const picojson::value& a: chat_active) {
                    if (!a.is<double>()) {
                        vkcom_debug_error("Strange response from messages.getDialogs: %s\n", v.serialize().data());
                        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
                    }
                    int64 id = a.get<double>();
                    if (id > 0) // E-mail participants are less than zero, let's just ignore them.
                        info.participants.push_back(id);
                }
            } else {
                if (!field_is_present<double>(message, "user_id")) {
                    vkcom_debug_error("Strange response from messages.getDialogs: %s\n", v.serialize().data());
                    purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
                    return;
                }

                uint64 user_id = message.get("user_id").get<double>();
                data->user_ids.insert(user_id);
            }
        }

        uint next_offset = offset + items.size();
        if (next_offset < count) {
            get_users_chats_from_dialogs_impl(gc, success_cb, data, next_offset);
        } else {
            conn_data->chat_ids = std::move(data->chat_ids);
            conn_data->dialog_user_ids = std::move(data->user_ids);
            success_cb();
        }
    }, [=](const picojson::value&) {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unable to retrieve dialogs list");
    });
}

void get_users_chats_from_dialogs(PurpleConnection* gc, const SuccessCb& success_cb)
{
    GetUsersChatsData_ptr data{ new GetUsersChatsData() };
    get_users_chats_from_dialogs_impl(gc, success_cb, data, 0);
}

void update_buddy_list(PurpleConnection* gc, bool update_presence)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    VkConnData* conn_data = get_conn_data(gc);

    // Check all currently known users if they should be added/updated to buddy list.
    for (const pair<uint64, VkUserInfo>& p: conn_data->user_infos) {
        uint64 user_id = p.first;
        if (!user_should_be_in_blist(gc, user_id))
            continue;

        update_buddy_in_blist(gc, user_id, p.second, update_presence);
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

        vkcom_debug_info("Removing %s from buddy list\n", purple_buddy_get_name(buddy));
        purple_blist_remove_buddy(buddy);
    }
    g_slist_free(buddies_list);

    // Check all currently known chats if they should be added/updated to buddy list.
    for (const pair<uint64, VkChatInfo>& p: conn_data->chat_infos) {
        uint64 chat_id = p.first;
        if (!chat_should_be_in_blist(gc, chat_id))
            continue;

        update_chat_in_blist(gc, chat_id, p.second);
    }

    // Check all current chats in buddyd list if they should be removed.
    vector<PurpleChat*> chats_list = find_all_purple_chats(account);
    for (PurpleChat* chat: chats_list) {
        const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
        if (!chat_name)
            continue;

        uint64 chat_id = chat_id_from_name(chat_name);
        if (chat_should_be_in_blist(gc, chat_id))
            continue;

        vkcom_debug_info("Removing chat%" PRIu64 " from buddy list\n", chat_id);
        purple_blist_remove_chat(chat);
    }
}

} // End anonymous namespace

void update_open_conversation_presence(PurpleConnection *gc)
{
    uint64_vec user_ids;
    VkConnData* conn_data = get_conn_data(gc);
    for (const auto& it: conn_data->user_infos) {
        uint64 user_id = it.first;
        if (!user_is_friend(gc, user_id) && find_conv_for_id(gc, user_id, 0))
            user_ids.push_back(user_id);
    }

    if (user_ids.empty())
        return;

    string user_ids_str = str_concat_int(',', user_ids);
    vkcom_debug_info("Updating online status for buddies %s\n", user_ids_str.data());

    CallParams params = { {"fields", "online,online_mobile"} };
    vk_call_api_ids(gc, "users.get", params, "user_ids", user_ids, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            vkcom_debug_error("Wrong type returned as users.get call result\n");
            return;
        }

        for (const picojson::value& v: result.get<picojson::array>()) {
            if (!v.is<picojson::object>()) {
                vkcom_debug_error("Strange node found in users.get result: %s\n",
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
            vkcom_debug_info("Got status %d, %d for %" PRIu64 "\n", online, online_mobile, user_id);

            VkUserInfo* info = get_user_info(gc, user_id);
            // We still have not updated info. It is highly unlikely, but still possible.
            if (!info)
                continue;
            if (info->online == online && info->online_mobile == online_mobile)
                continue;

            info->online = online;
            info->online_mobile = online_mobile;
            update_buddy_presence_internal(gc, user_name_from_id(user_id), *info);
        }
    }, nullptr, nullptr);
}

void update_presence_in_buddy_list(PurpleConnection *gc, uint64 user_id)
{
    VkUserInfo* info = get_user_info(gc, user_id);
    // We still haven't updated info.
    if (!info)
        return;

    update_buddy_presence_internal(gc, user_name_from_id(user_id), *info);
}


void check_custom_alias_group(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    for (PurpleBlistNode* node = purple_blist_get_root(); node; node = purple_blist_node_next(node, FALSE)) {
        if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
            PurpleBuddy* buddy = PURPLE_BUDDY(node);
            if (purple_buddy_get_account(buddy) != account)
                continue;

            check_buddy_alias(gc, buddy);
            check_buddy_group(gc, buddy);
        } else if (PURPLE_BLIST_NODE_IS_CHAT(node)) {
            PurpleChat* chat = PURPLE_CHAT(node);
            if (purple_chat_get_account(chat) != account)
                continue;

            check_chat_alias(gc, chat);
            check_chat_group(gc, chat);
        }
    }
}
