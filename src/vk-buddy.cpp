#include <debug.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"

#include "vk-buddy.h"

namespace
{

// Helper callback, used for friends.get and users.get. Updates all buddies info from the result,
// returns names of all buddies.
// friends_get must be true if the function is called for friends.get result, false for users.get
// (these two methods have actually slightly different response objects).
string_set on_update_buddy_list(PurpleConnection* gc, const picojson::value& result, bool friends_get, bool update_presence);

// Removes all buddies, not present in string_set, from buddy list.
void clean_buddy_list(PurpleConnection* gc, const string_set& buddy_names);

} // End of anonymous namespace

const char user_fields_param[] = "first_name,last_name,bdate,education,photo_50,photo_max_orig,"
                                 "online,contacts,can_write_private_message,activity,last_seen,domain";

void update_buddy_list(PurpleConnection* gc, const BuddyUpdatedCb& on_update_cb, bool update_presence)
{
    purple_debug_info("prpl-vkcom", "Updating full buddy list\n");

    VkConnData* data = get_conn_data(gc);
    CallParams params = { {"user_id", to_string(data->uid())}, {"fields", user_fields_param} };
    vk_call_api(gc, "friends.get", params, [=](const picojson::value& result) {
        clean_buddy_list(gc, on_update_buddy_list(gc, result, true, update_presence));
        if (on_update_cb)
            on_update_cb();
    });
}

void update_buddy(PurpleConnection* gc, uint64 uid, const BuddyUpdatedCb& on_update_cb, bool update_presence)
{
    purple_debug_info("prpl-vkcom", "Updating information for buddy %llu\n", (unsigned long long)uid);

    CallParams params = { {"user_ids", to_string(uid)}, {"fields", user_fields_param} };
    vk_call_api(gc, "users.get", params, [=](const picojson::value& result) {
        on_update_buddy_list(gc, result, false, update_presence);
        if (on_update_cb)
            on_update_cb();
    });
}

namespace
{

// Updates information about buddy from given object and returns buddy name.
string update_buddy_from_object(PurpleConnection* gc, const picojson::value& v, bool update_presence);

string_set on_update_buddy_list(PurpleConnection* gc, const picojson::value& result, bool friends_get,
                                bool update_presence)
{
    if (friends_get && !result.is<picojson::object>()) {
        purple_debug_error("prpl-vkcom", "Wrong type returned as friends.get call result\n");
        return string_set();
    }

    const picojson::value& items = friends_get ? result.get("items") : result;
    if (!items.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Wrong type returned as friends.get or users.get call result\n");
        return string_set();
    }

    // Adds or updates buddies in result and forms the active set of buddy ids.
    string_set buddy_names;
    for (const picojson::value& v: items.get<picojson::array>()) {
        if (!v.is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Strange node found in friends.get or users.get result: %s\n",
                               v.serialize().data());
            continue;
        }
        string name = update_buddy_from_object(gc, v, update_presence);
        if (!name.empty())
            buddy_names.insert(name);
    }

    return buddy_names;
}

void clean_buddy_list(PurpleConnection* gc, const string_set& buddy_names)
{
    purple_debug_info("prpl-vkcom", "Cleaning old entries in buddy list\n");

    PurpleAccount* account = purple_connection_get_account(gc);
    GSList* buddies_list = purple_find_buddies(account, nullptr);
    for (GSList* it = buddies_list; it; it = it->next) {
        PurpleBuddy* buddy = (PurpleBuddy*)it->data;
        if (!ccontains(buddy_names, purple_buddy_get_name(buddy))) {
            purple_debug_info("prpl-vkcom", "Removing %s from buddy list\n", purple_buddy_get_name(buddy));
            purple_blist_remove_buddy(buddy);
        }
    }
    g_slist_free(buddies_list);
}

// Converts a few html entities back into plaintext: &amp; &gt; &lt; &apos; &quot; &ndash; &mdash;
// Hilariously ugly, todo is using re2c or other finite automata generator.
string replace_html_entities(const string& s);

// Creates single string from multiple fields in user_fields, describing education.
string make_education_string(const picojson::value& v);

// Sets buddy icon.
void on_fetch_buddy_icon_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, PurpleAccount* account,
                            const string& name);

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

void on_fetch_buddy_icon_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, PurpleAccount* account,
                            const string& name)
{
    purple_debug_info("prpl-vkcom", "Updating buddy icon for %s\n", name.data());
    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error while fetching buddy icon: %s\n", purple_http_response_get_error(response));
        return;
    }

    size_t icon_len;
    const void* icon_data = purple_http_response_get_data(response, &icon_len);
    const char* icon_url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    purple_buddy_icons_set_for_user(account, name.data(), g_memdup(icon_data, icon_len),
                                    icon_len, icon_url);
}

string update_buddy_from_object(PurpleConnection* gc, const picojson::value& v, bool update_presence)
{
    if (field_is_present<string>(v, "deactivated"))
        return "";

    if (!field_is_present<double>(v, "id")
            || !field_is_present<string>(v, "first_name")
            || !field_is_present<string>(v, "last_name")
            || !field_is_present<double>(v, "online")
            || !field_is_present<string>(v, "photo_50")
            || !field_is_present<string>(v, "photo_max_orig")
            || !field_is_present<picojson::object>(v, "last_seen")
            || !field_is_present<string>(v, "activity")
            || !field_is_present<double>(v, "can_write_private_message")) {
        purple_debug_error("prpl-vkcom", "Incomplete user information in friends.get or users.get: %s\n",
                           picojson::value(v).serialize().data());
        return "";
    }

    if (v.get("can_write_private_message").get<double>() != 1)
        return "";

    // All buddy names are always in the form "idXXXXX" disregarding real name or nickname.
    uint64 uid = v.get("id").get<double>();
    string name = buddy_name_from_uid(uid);
    // Buddy "real" name.
    string alias = v.get("first_name").get<string>() + " " + v.get("last_name").get<string>();

    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, name.data());
    if (!buddy) {
        purple_debug_info("prpl-vkcom", "Adding %s to buddy list\n", name.data());
        buddy = purple_buddy_new(account, name.data(), nullptr);
        purple_blist_add_buddy(buddy, nullptr, nullptr, nullptr);
    }

    // Set "server alias"
    serv_got_alias(gc, name.data(), alias.data());
    // Set "client alias", the one that is stored in blist on the client and can be set by the user.
    // If we do not set it, the ugly "idXXXX" entries will appear in buddy list during connection.
    purple_serv_got_private_alias(gc, name.data(), alias.data());

    // Store all data, required for get_info, tooltip_text etc.
    VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);
    if (!data) {
        data = new VkBuddyData;
        purple_buddy_set_protocol_data(buddy, data);
    }

    data->uid = v.get("id").get<double>();
    data->activity = unescape_html(v.get("activity").get<string>());
    if (field_is_present<string>(v, "bdate"))
        data->bdate = unescape_html(v.get("bdate").get<string>());
    data->education = unescape_html(make_education_string(v));
    data->photo_max = v.get("photo_max_orig").get<string>();
    if (field_is_present<string>(v, "mobile_phone"))
        data->mobile_phone = unescape_html(v.get("mobile_phone").get<string>());
    data->domain = v.get("domain").get<string>();
    if (field_is_present<double>(v, "online_mobile"))
        data->is_mobile = true;
    else
        data->is_mobile = false;

    // Update login time.
    bool user_online = v.get("online").get<double>() == 1;
    if (user_online) {
        int login_time = v.get("last_seen").get("time").get<double>();
        if (login_time != 0)
            purple_prpl_got_user_login_time(account, name.data(), login_time);
        else
            purple_debug_error("prpl-vkcom", "Zero login time for %s\n", name.data());
    }

    // Update presence
    if (update_presence) {
        purple_prpl_got_user_status(account, name.data(), user_online ? "online" : "offline", nullptr);
    } else {
        // We do not update online/offline status here, because it is done in Long Poll processing but we
        // "update" it so that status strings in buddy list get updated (vk_status_text gets called).
        PurpleStatus* status = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
        purple_prpl_got_user_status(account, name.data(), purple_status_get_id(status), nullptr);
    }

    // Buddy icon. URL is used as a "checksum".
    const string& photo_url = v.get("photo_50").get<string>();
    static const char empty_photo_a[] = "http://vkontakte.ru/images/camera_ab.gif";
    static const char empty_photo_b[] = "http://vkontakte.ru/images/camera_b.gif";
    if (photo_url == empty_photo_a || photo_url == empty_photo_b) {
        purple_buddy_icons_set_for_user(account, name.data(), nullptr, 0, nullptr);
    } else {
        const char* checksum = purple_buddy_icons_get_checksum_for_user(buddy);
        if (!checksum || checksum != photo_url) {
            http_get(gc, photo_url, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
                on_fetch_buddy_icon_cb(http_conn, response, account, name);
            });
        }
    }

    return name;
}

} // End anonymous namespace


void get_buddy_full_name(PurpleConnection* gc, uint64 uid, const NameFetchedCb& fetch_cb)
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
