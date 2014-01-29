#include <debug.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-buddy.h"

namespace
{

using std::move;

// Helper callback, used for friends.get and users.get. Adds and/or updates all buddies info in
// VkConnData->user_infos from the result. Returns list of uids.
//
// friends_get must be true if the function is called for friends.get result, false for users.get
// (these two methods have actually slightly different response objects);
// update_presence has the same meaning as in update_buddy_list
uint64_set on_update_user_infos(PurpleConnection* gc, const picojson::value& result, bool friends_get,
                                bool update_presence);

// Returns a set of uids of all non-friends, which a user had a dialog with.
using ReceivedUsersCb = std::function<void(const uint64_set&)>;
void get_users_from_dialogs(PurpleConnection* gc, ReceivedUsersCb received_users_cb);

// Updates buddy list according to friend_uids and user_infos stored in VkConnData. Adds new buddies, removes
// old buddies, updates buddy aliases and avatars. Buddy icons (avatars) are updated asynchronously.
void update_buddy_list(PurpleConnection* gc, bool update_presence);

// Checks if uids are present in buddy list and adds them if they are not. Ignores "friends only in buddy list"
// setting.
void update_buddy_list_for(PurpleConnection* gc, const uint64_vec& uids, bool update_presence);

} // End of anonymous namespace

const char user_fields_param[] = "first_name,last_name,bdate,education,photo_50,photo_max_orig,"
                                 "online,contacts,can_write_private_message,activity,last_seen,domain";

void update_buddies(PurpleConnection* gc, bool update_presence, const SuccessCb& on_update_cb)
{
    purple_debug_info("prpl-vkcom", "Updating full buddy list\n");

    VkConnData* conn_data = get_conn_data(gc);
    CallParams params = { {"user_id", to_string(conn_data->uid())}, {"fields", user_fields_param} };
    vk_call_api(gc, "friends.get", params, [=](const picojson::value& result) {
        conn_data->friend_uids = on_update_user_infos(gc, result, true, update_presence);
        get_users_from_dialogs(gc, [=](const uint64_set& dialog_uids) {
            uint64_vec non_friend_uids;
            if (!purple_account_get_bool(purple_connection_get_account(gc), "only_friends_in_blist", false)) {
                for (uint64 user_id: dialog_uids)
                    if (!is_friend(gc, user_id))
                        non_friend_uids.push_back(user_id);
            }
            for (uint64 user_id: conn_data->manually_added_buddies)
                // We could've manually added buddy and he has become our friend later.
                if (!is_friend(gc, user_id))
                    non_friend_uids.push_back(user_id);

            add_or_update_user_infos(gc, non_friend_uids, [=] {
                update_buddy_list(gc, update_presence);
                if (on_update_cb)
                    on_update_cb();
            });
        });
    });
}

void add_or_update_user_infos(PurpleConnection* gc, const uint64_vec& uids, const SuccessCb& on_update_cb)
{
    if (uids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    for (uint64 uid: uids)
        assert(!is_friend(gc, uid));

    string ids_str = str_concat_int(',', uids);
    purple_debug_info("prpl-vkcom", "Updating information for buddies %s\n", ids_str.data());

    CallParams params = { {"user_ids", ids_str}, {"fields", user_fields_param} };
    vk_call_api(gc, "users.get", params, [=](const picojson::value& result) {
        on_update_user_infos(gc, result, false, true);
        if (on_update_cb)
            on_update_cb();
    });
}

void add_buddies_if_needed(PurpleConnection* gc, const uint64_vec& uids, const SuccessCb& on_update_cb)
{
    if (uids.empty()) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    uint64_vec unknown_uids;
    for (uint64 uid: uids)
        if (is_unknown_uid(gc, uid))
            unknown_uids.push_back(uid);

    add_or_update_user_infos(gc, unknown_uids, [=] {
        update_buddy_list_for(gc, uids, true);
        if (on_update_cb)
            on_update_cb();
    });
}

void add_buddy_if_needed(PurpleConnection* gc, uint64 user_id, const SuccessCb& on_update_cb)
{
    if (in_buddy_list(gc, user_id)) {
        if (on_update_cb)
            on_update_cb();
        return;
    }

    add_buddies_if_needed(gc, { user_id }, on_update_cb);
}

namespace
{

// Updates user info about buddy and returns buddy uid or zero in case of failure.
uint64 on_update_user_info(PurpleConnection* gc, const picojson::value& fields, bool update_presence);

uint64_set on_update_user_infos(PurpleConnection* gc, const picojson::value& result, bool friends_get, bool update_presence)
{
    if (friends_get && !result.is<picojson::object>()) {
        purple_debug_error("prpl-vkcom", "Wrong type returned as friends.get call result\n");
        return {};
    }

    const picojson::value& items = friends_get ? result.get("items") : result;
    if (!items.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Wrong type returned as friends.get or users.get call result\n");
        return {};
    }

    // Adds or updates buddies in result and forms the active set of buddy ids.
    uint64_set buddy_uids;
    for (const picojson::value& v: items.get<picojson::array>()) {
        if (!v.is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Strange node found in friends.get or users.get result: %s\n",
                               v.serialize().data());
            continue;
        }
        uint64 uid = on_update_user_info(gc, v, update_presence);
        if (uid != 0)
            buddy_uids.insert(uid);
    }

    return buddy_uids;
}

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

uint64 on_update_user_info(PurpleConnection* gc, const picojson::value& fields, bool update_presence)
{
    if (!field_is_present<double>(fields, "id")
            || !field_is_present<string>(fields, "first_name")
            || !field_is_present<string>(fields, "last_name")) {
        purple_debug_error("prpl-vkcom", "Incomplete user information in friends.get or users.get: %s\n",
                           picojson::value(fields).serialize().data());
        return 0;
    }
    uint64 uid = fields.get("id").get<double>();

    VkConnData* conn_data = get_conn_data(gc);
    VkUserInfo& info = conn_data->user_infos[uid];
    info.name = fields.get("first_name").get<string>() + " " + fields.get("last_name").get<string>();

    // We cannot write private messages, we have zero interest in user.
    if (field_is_present<string>(fields, "deactivated")
            || fields.get("can_write_private_message").get<double>() != 1) {
        info.can_write = false;
        return 0;
    }

    if (field_is_present<string>(fields, "photo_50")) {
        info.photo_min = fields.get("photo_50").get<string>();
        static const char empty_photo_a[] = "http://vkontakte.ru/images/camera_a.gif";
        static const char empty_photo_b[] = "http://vkontakte.ru/images/camera_b.gif";
        if (info.photo_min == empty_photo_a || info.photo_min == empty_photo_b)
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

    bool online = false;
    if (field_is_present<double>(fields, "online"))
        online = fields.get("online").get<double>() == 1;

    bool online_mobile = field_is_present<double>(fields, "online_mobile");

    if (update_presence) {
        info.online = online;
        info.online_mobile = online_mobile;
    } else {
        if (info.online != online || info.online_mobile != online_mobile)
            purple_debug_error("prpl-vkcom", "Strange, got different online status"
                               "in friends.get vs Long Poll: %d, %d vs %d, %d\n",
                               online, online_mobile, info.online, info.online_mobile);
    }

    if (field_is_present<picojson::object>(fields, "last_seen"))
        info.last_seen = fields.get("last_seen").get("time").get<double>();

    return uid;
}

void get_users_from_dialogs(PurpleConnection* gc, ReceivedUsersCb received_users_cb)
{
    struct Helper
    {
        uint64_set uids;
        ReceivedUsersCb received_users_cb;
    };
    shared_ptr<Helper> helper{ new Helper{ {}, move(received_users_cb) } };

    // preview_length minimum value is 1, zero means "full message".
    CallParams params = { {"preview_length", "1"}, {"count", "200"} };
    vk_call_api_items(gc, "messages.getDialogs", params, true, [=](const picojson::value& dialog) {
        if (!field_is_present<double>(dialog, "user_id")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.getDialogs: %s\n",
                               dialog.serialize().data());
            return;
        }

        uint64 uid = dialog.get("user_id").get<double>();
        helper->uids.insert(uid);
    }, [=] {
        helper->received_users_cb(helper->uids);
    }, [=](const picojson::value&) {
        helper->received_users_cb({});
    });
}

// Returns true if buddy with given user id should be shown in buddy list, false otherwise.
bool should_be_in_blist(PurpleConnection* gc, uint64 user_id)
{
    if (have_conversation_with(gc, user_id))
        return true;
    if (is_manually_removed(gc, user_id))
        return false;

    PurpleAccount* account = purple_connection_get_account(gc);
    bool friends_only = purple_account_get_bool(account, "only_friends_in_blist", false);
    if (friends_only) {
        if (is_friend(gc, user_id))
            return true;
        if (is_manually_added(gc, user_id))
            return true;
        return false;
    } else {
        return true;
    }
}

// Helper function for update_buddy_list and update_buddy_list_for
void update_buddy_in_blist(PurpleConnection* gc, uint64 uid, const VkUserInfo& info, bool update_presence);

void update_buddy_list(PurpleConnection* gc, bool update_presence)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    VkConnData* conn_data = get_conn_data(gc);
    // Check all currently known users if they should be added/updated to buddy list.
    for (const pair<uint64, VkUserInfo>& p: conn_data->user_infos) {
        uint64 user_id = p.first;
        if (!should_be_in_blist(gc, user_id))
            continue;

        update_buddy_in_blist(gc, user_id, p.second, update_presence);
    }

    // Check all current buddy list entries if they should be removed.
    GSList* buddies_list = purple_find_buddies(account, nullptr);
    for (GSList* it = buddies_list; it; it = it->next) {
        PurpleBuddy* buddy = (PurpleBuddy*)it->data;
        uint64 user_id = uid_from_buddy_name(purple_buddy_get_name(buddy));

        if (contains_key(conn_data->user_infos, user_id) && should_be_in_blist(gc, user_id))
            continue;

        purple_debug_info("prpl-vkcom", "Removing %s from buddy list\n", purple_buddy_get_name(buddy));
        purple_blist_remove_buddy(buddy);
    }
    g_slist_free(buddies_list);
}

void update_buddy_list_for(PurpleConnection* gc, const uint64_vec& uids, bool update_presence)
{
    VkConnData* conn_data = get_conn_data(gc);
    for (uint64 uid: uids)
        update_buddy_in_blist(gc, uid, conn_data->user_infos[uid], update_presence);
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

// Updates buddy status. It is a bit more complicated than simply calling purple_prpl_got_user_status,
// because we want to make sure buddy->icon is set. It is used e.g. in libnotify notifications
// "Buddy signed in". It is loaded lazily by Pidgin buddy list (this applies to all protocols,
// not only vkcom) and will not be loaded e.g. until buddy comes online (it will be loaded AFTER
// libnotify shows notification).
void update_buddy_status_internal(PurpleConnection* gc, const string& buddy_name, const VkUserInfo& info)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());

    // Check if icon has not been already loaded.
    if (!purple_buddy_get_icon(buddy))
        // This method forces icons to be loaded.
        purple_buddy_icons_find(account, buddy_name.data());
    purple_prpl_got_user_status(account, buddy_name.data(), get_user_status(info), nullptr);
}

// Returns default group to add buddies to.
PurpleGroup* get_default_group(PurpleConnection* gc);

// Starts downloading buddy icon and sets it upon finishing.
void fetch_buddy_icon(PurpleConnection* gc, const string& buddy_name, const string& icon_url);

void update_buddy_in_blist(PurpleConnection* gc, uint64 uid, const VkUserInfo& info, bool update_presence)
{
    PurpleAccount* account = purple_connection_get_account(gc);

    string buddy_name = buddy_name_from_uid(uid);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());
    PurpleGroup* group = get_default_group(gc);
    if (!buddy) {
        purple_debug_info("prpl-vkcom", "Adding %s to buddy list\n", buddy_name.data());
        buddy = purple_buddy_new(account, buddy_name.data(), nullptr);

        purple_blist_add_buddy(buddy, nullptr, group, nullptr);
    } else if (group) {
        // User set Group for Buddies. Check user has not set custom group for this particular buddy
        // and buddy is in the Group for Buddies.
        PurpleGroup* old_group = purple_buddy_get_group(buddy);
        if (!g_str_equal(group->name, old_group->name)
                && !purple_blist_node_get_bool(&buddy->node, "custom-group"))
        {
            purple_debug_info("prpl-vkcom", "Moving %s from %s to %s\n", buddy_name.data(),
                              old_group->name, group->name);
            // add_buddy moves existing buddies from group to group.
            purple_blist_add_buddy(buddy, nullptr, group, nullptr);
        }
    }

    // Check if user did not set alias for this particular buddy locally.
    if (!purple_blist_node_get_bool(&buddy->node, "custom-alias")) {
        // Check if name has already been set, so that we do not get spurious "idXXXX is now known as ..."
        if (info.name != purple_buddy_get_alias(buddy))
        {
            // Pidgin supports two types of aliases for buddies: "local"/"private" and "server". The local alias
            // is permanently stored in the buddy list and can be modified by the user (when the user modifies
            // some buddy alias, we set "custom-alias" for that node). The server alias is ephemeral and must be
            // set upon each login. Local status is considered dominant to server status. The only reason
            // why we call serv_got_alias is because that function conveniently writes "idXXXX is now known as YYY"
            // if conversation with that buddy is open. Otherwise, server alias is completely ignored.
            // 
            // TODO: we could use server status to remove "custom-alias" tag from node if local alias is set to 
            // server alias.
            purple_serv_got_private_alias(gc, buddy_name.data(), info.name.data());
        }
    }

    // Update presence
    if (update_presence) {
        update_buddy_status_internal(gc, buddy_name, info);
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
            purple_debug_error("prpl-vkcom", "Zero login time for %s\n", buddy_name.data());
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

PurpleGroup* get_default_group(PurpleConnection* gc)
{
    const char* group_name = purple_account_get_string(purple_connection_get_account(gc),
                                                       "blist_default_group", "");
    if (group_name && group_name[0] != '\0')
        return purple_group_new(group_name);
    else
        return nullptr;
}

void fetch_buddy_icon(PurpleConnection* gc, const string& buddy_name, const string& icon_url)
{
    http_get(gc, icon_url, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        purple_debug_info("prpl-vkcom", "Updating buddy icon for %s\n", buddy_name.data());
        if (!purple_http_response_is_successful(response)) {
            purple_debug_error("prpl-vkcom", "Error while fetching buddy icon: %s\n",
                               purple_http_response_get_error(response));
            return;
        }

        size_t icon_len;
        const void* icon_data = purple_http_response_get_data(response, &icon_len);
        const char* icon_url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
        // This should be synchronized with code in update_buddy_in_blist.
        string checksum = str_rsplit(icon_url, '/');
        purple_buddy_icons_set_for_user(purple_connection_get_account(gc), buddy_name.data(),
                                        g_memdup(icon_data, icon_len), icon_len, checksum.data());
    });
}

} // End anonymous namespace

void update_buddies_presence_only(PurpleConnection* gc, const uint64_vec user_ids, const SuccessCb& on_update_cb)
{
    string ids_str = str_concat_int(',', user_ids);
    purple_debug_info("prpl-vkcom", "Updating online status for buddies %s\n", ids_str.data());
    CallParams params = { {"user_ids", ids_str}, {"fields", "online,online_mobile"} };
    vk_call_api(gc, "users.get", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result\n");
            return;
        }

        VkConnData* conn_data = get_conn_data(gc);
        for (const picojson::value& v: result.get<picojson::array>()) {
            if (!v.is<picojson::object>()) {
                purple_debug_error("prpl-vkcom", "Strange node found in users.get result: %s\n",
                                   v.serialize().data());
                continue;
            }
            if (!field_is_present<double>(v, "id") || !field_is_present<double>(v, "online")) {
                purple_debug_error("prpl-vkcom", "Strange node found in users.get result: %s\n",
                                   v.serialize().data());
                continue;
            }
            uint64 user_id = v.get("id").get<double>();
            bool online = v.get("online").get<double>() == 1;
            bool online_mobile = field_is_present<double>(v, "online_mobile");

            VkUserInfo& info = conn_data->user_infos[user_id];
            if (info.online == online && info.online_mobile == online_mobile)
                return;

            info.online = online;
            info.online_mobile = online_mobile;
            update_buddy_status_internal(gc, buddy_name_from_uid(user_id), info);
        }
        if (on_update_cb)
            on_update_cb();
    });

}

void update_open_conversation_presence(PurpleConnection *gc)
{
    uint64_vec open_non_friends;
    VkConnData* conn_data = get_conn_data(gc);
    for (const auto& it: conn_data->user_infos) {
        uint64 user_id = it.first;
        if (!is_friend(gc, user_id) && find_conv_for_id(gc, user_id, 0))
            open_non_friends.push_back(user_id);
    }

    if (open_non_friends.empty())
        return;

    update_buddies_presence_only(gc, open_non_friends);
}

void remove_buddy_if_needed(PurpleConnection* gc, uint64 user_id)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    if (should_be_in_blist(gc, user_id))
        return;

    string buddy_name = buddy_name_from_uid(user_id);
    PurpleBuddy* buddy = purple_find_buddy(account, buddy_name.data());
    if (!buddy) {
        purple_debug_info("prpl-vkcom", "Trying to remove buddy %s not in buddy list\n",
                          buddy_name.data());
        return;
    }

    purple_debug_info("prpl-vkcom", "Removing %s from buddy list as unneeded after convo close\n",
                      buddy_name.data());
    purple_blist_remove_buddy(buddy);
}


void get_user_full_name(PurpleConnection* gc, uint64 uid, const NameFetchedCb& fetch_cb)
{
    purple_debug_info("prpl-vkcom", "Getting full name for %llu\n", (unsigned long long)uid);

    CallParams params = { {"user_ids", to_string(uid)}, {"fields", "first_name,last_name"} };
    vk_call_api(gc, "users.get", params, [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }

        const picojson::array& users = result.get<picojson::array>();
        if (users.size() != 1) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }

        if (!field_is_present<string>(users[0], "first_name") || !field_is_present<string>(users[0], "last_name")) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().data());
            return;
        }
        string first_name = users[0].get("first_name").get<string>();
        string last_name = users[0].get("last_name").get<string>();

        fetch_cb(first_name + " " + last_name);
    });
}


void find_user_by_screenname(PurpleConnection* gc, const string& screen_name, const UidFetchedCb& fetch_cb)
{
    purple_debug_info("prpl-vkcom", "Finding user id for %s\n", screen_name.data());

    CallParams params = { {"screen_name", screen_name} };
    vk_call_api(gc, "utils.resolveScreenName", params, [=](const picojson::value& result) {
        if (!field_is_present<string>(result, "type") || !field_is_present<double>(result, "object_id")) {
            purple_debug_error("prpl-vkcom", "Unable to find user matching %s\n", screen_name.data());
            fetch_cb(0);
            return;
        }

        if (result.get("type").get<string>() != "user") {
            purple_debug_error("prpl-vkcom", "Type of %s is %s\n", screen_name.data(),
                               result.get("type").get<string>().data());
            fetch_cb(0);
            return;
        }

        uint64 uid = result.get("object_id").get<double>();
        fetch_cb(uid);
    }, [=](const picojson::value&) {
        fetch_cb(0);
    });
}
