#include "vk-buddy.h"

#include <debug.h>

#include "httputils.h"
#include "vk-api.h"
#include "vk-common.h"
#include "utils.h"

namespace
{

// Helper callback, used for friends.get and users.get. Updates all buddies info from the result,
// returns names of all buddies.
// friends_get must be true if the function is called for friends.get result, false for users.get
// (these two methods have actually slightly different response objects).
string_set on_update_buddy_list(PurpleConnection* gc, const picojson::value& result, bool friends_get);

// Removes all buddies, not present in string_set, from buddy list.
void clean_buddy_list(PurpleConnection* gc, const string_set& buddy_names);

} // End of anonymous namespace

const char user_fields_param[] = "first_name,last_name,bdate,education,photo_50,photo_max_orig,"
                                 "online,contacts,can_write_private_message,activity,last_seen,domain";

void update_buddy_list(PurpleConnection* gc, const OnUpdateCb& on_update_cb)
{
    purple_debug_info("prpl-vkcom", "Updating full buddy list\n");

    VkConnData* data = (VkConnData*)purple_connection_get_protocol_data(gc);
    string_map params = { {"user_id", data->uid()}, {"fields", user_fields_param} };
    vk_call_api(gc, "friends.get", params, data->access_token(), [=](const picojson::value& result) {
        clean_buddy_list(gc, on_update_buddy_list(gc, result, true));
        if (on_update_cb)
            on_update_cb();
    }, nullptr);
}

void update_buddy(PurpleConnection* gc, const string& id, const OnUpdateCb& on_update_cb)
{
    purple_debug_info("prpl-vkcom", "Updating information for buddy %s\n", id.c_str());

    VkConnData* data = (VkConnData*)purple_connection_get_protocol_data(gc);
    string_map params = { {"user_ids", id}, {"fields", user_fields_param} };
    vk_call_api(gc, "users.get", params, data->access_token(), [=](const picojson::value& result) {
        on_update_buddy_list(gc, result, false);
        if (on_update_cb)
            on_update_cb();
    }, nullptr);
}

namespace
{

// Updates information about buddy from given object and returns buddy name.
string update_buddy_from_object(PurpleConnection* gc, const picojson::object& user_fields);

string_set on_update_buddy_list(PurpleConnection* gc, const picojson::value& result, bool friends_get)
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
    for (const picojson::value& user_fields: items.get<picojson::array>()) {
        if (!user_fields.is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Strange node found in friends.get or users.get result: %s\n",
                               user_fields.serialize().c_str());
            continue;
        }
        string name = update_buddy_from_object(gc, user_fields.get<picojson::object>());
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
string make_education_string(const picojson::object& user_fields);

// Sets buddy icon.
void on_fetch_buddy_icon_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, PurpleAccount* account,
                            const string& name);

string replace_html_entities(const string& s)
{
    string ret;
    int len = s.length();
    ret.reserve(s.length());
    for (int i = 0; i < len; ++i) {
        if (s[i] != '&') {
            ret.append(1, s[i]);
        } else {
            const char* p = s.data() + i;
            if (len - i >= 7) { // mdash, ndash
                if (strncmp(p, "&mdash;", 7) == 0 || strncmp(p, "&ndash;", 7) == 0) {
                    ret.append(1, '-');
                    i += 6;
                    continue;
                }
            }
            if (len - i >= 6) { // apos, quot
                if (strncmp(p, "&apos;", 6) == 0) {
                    ret.append(1, '\'');
                    i += 5;
                    continue;
                } else if (strncmp(p, "&quot;", 6) == 0) {
                    ret.append(1, '"');
                    i += 5;
                    continue;
                }
            }
            if (len - i >= 5) { // amp
                if (strncmp(p, "&amp;", 5) == 0) {
                    ret.append(1, '&');
                    i += 4;
                    continue;
                }
            }
            if (len - i >= 4) { // lt, gt
                if (strncmp(p, "&lt;", 4) == 0) {
                    ret.append(1, '<');
                    i += 3;
                    continue;
                } else if (strncmp(p, "&gt;", 4) == 0) {
                    ret.append(1, '>');
                    i += 3;
                    continue;
                }
            }
            break; // Let's skip all the escape sequence
        }
    }
    return ret;
}

string make_education_string(const picojson::object& user_fields)
{
    string ret;
    if (field_is_present<string>(user_fields, "university_name")) {
        ret = user_fields.at("university_name").get<string>();
        if (ret.empty())
            return ret;
        if (field_is_present<string>(user_fields, "faculty_name"))
            ret = user_fields.at("faculty_name").get<string>() +  ", " + ret;
        if (field_is_present<double>(user_fields, "graduation")) {
            int graduation = int(user_fields.at("graduation").get<double>());
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
    purple_debug_info("prpl-vkcom", "Updating buddy icon for %s", name.c_str());
    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error while fetching buddy icon: %s", purple_http_response_get_error(response));
        return;
    }

    size_t icon_len;
    const void* icon_data = purple_http_response_get_data(response, &icon_len);
    const char* icon_url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    purple_buddy_icons_set_for_user(account, name.c_str(), g_memdup(icon_data, icon_len),
                                    icon_len, icon_url);
}

string update_buddy_from_object(PurpleConnection* gc, const picojson::object& user_fields)
{
    if (field_is_present<string>(user_fields, "deactivated"))
        return "";

    if (!field_is_present<double>(user_fields, "id")
            || !field_is_present<string>(user_fields, "first_name")
            || !field_is_present<string>(user_fields, "last_name")
            || !field_is_present<double>(user_fields, "online")
            || !field_is_present<string>(user_fields, "photo_50")
            || !field_is_present<string>(user_fields, "photo_max_orig")
            || !field_is_present<picojson::object>(user_fields, "last_seen")
            || !field_is_present<string>(user_fields, "activity")
            || !field_is_present<double>(user_fields, "can_write_private_message")) {
        purple_debug_error("prpl-vkcom", "Incomplete user information in friends.get or users.get: %s\n",
                           picojson::value(user_fields).serialize().c_str());
        return "";
    }

    if (user_fields.at("can_write_private_message").get<double>() != 1)
        return "";

    PurpleAccount* account = purple_connection_get_account(gc);

    // All buddy names are always in the form "idXXXXX" disregarding real name or nickname.
    string name = "id" + user_fields.at("id").to_str();
    // Buddy "real" name.
    string alias = user_fields.at("first_name").get<string>() + " " + user_fields.at("last_name").get<string>();

    PurpleBuddy* buddy = purple_find_buddy(account, name.c_str());
    if (!buddy) {
        purple_debug_info("prpl-vkcom", "Adding %s to buddy list\n", name.c_str());
        buddy = purple_buddy_new(account, name.c_str(), nullptr);
        purple_blist_add_buddy(buddy, nullptr, nullptr, nullptr);
    }

    // Set "server alias"
    serv_got_alias(gc, name.c_str(), alias.c_str());
    // Set "client alias", the one that is stored in blist on the client and can be set by the user.
    // If we do not set it, the ugly "idXXXX" entries will appear in buddy list during connection.
    purple_serv_got_private_alias(gc, name.c_str(), alias.c_str());

    // Store all data, required for get_info, tooltip_text etc.
    VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);
    if (!data) {
        data = new VkBuddyData;
        purple_buddy_set_protocol_data(buddy, data);
    }

    data->uid = user_fields.at("id").to_str();
    data->activity = replace_html_entities(user_fields.at("activity").get<string>());
    if (field_is_present<string>(user_fields, "bdate"))
        data->bdate = replace_html_entities(user_fields.at("bdate").get<string>());
    data->education = replace_html_entities(make_education_string(user_fields));
    data->photo_max = user_fields.at("photo_max_orig").get<string>();
    if (field_is_present<string>(user_fields, "mobile_phone"))
        data->mobile_phone = replace_html_entities(user_fields.at("mobile_phone").get<string>());
    data->domain = user_fields.at("domain").get<string>();
    if (ccontains(user_fields, "online_mobile"))
        data->is_mobile = true;
    else
        data->is_mobile = false;

    // Buddy status text and login time. This should be called after filling VkBuddyData
    // because setting status triggers vk_status_text, which uses VkBuddyData.
    if (user_fields.at("online").get<double>() == 1) {
        purple_prpl_got_user_status(account, name.c_str(), "online", nullptr);

        int login_time = user_fields.at("last_seen").get("time").get<double>();
        if (login_time != 0)
            purple_prpl_got_user_login_time(account, name.c_str(), login_time);
        else
            purple_debug_error("prpl-vkcom", "Zero login time for %s\n", name.c_str());
    } else {
        purple_prpl_got_user_status(account, name.c_str(), "offline", nullptr);
    }

    // Buddy icon. URL is used as a "checksum".
    const string& photo_url = user_fields.at("photo_50").get<string>();
    static const char empty_photo_a[] = "http://vkontakte.ru/images/camera_ab.gif";
    static const char empty_photo_b[] = "http://vkontakte.ru/images/camera_b.gif";
    if (photo_url == empty_photo_a || photo_url == empty_photo_b) {
        purple_buddy_icons_set_for_user(account, name.c_str(), nullptr, 0, nullptr);
    } else {
        const char* checksum = purple_buddy_icons_get_checksum_for_user(buddy);
        if (!checksum || checksum != photo_url) {
            http_get(gc, photo_url.c_str(), [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
                on_fetch_buddy_icon_cb(http_conn, response, account, "id" + data->uid);
            });
        }
    }

    return name;
}

} // End anonymous namespace


void get_buddy_full_name(PurpleConnection* gc, const string& id, FetchCb success_cb)
{
    purple_debug_info("prpl-vkcom", "Getting full name for %s\n", id.c_str());

    VkConnData* data = (VkConnData*)purple_connection_get_protocol_data(gc);
    string_map params = { {"user_ids", id}, {"fields", "first_name,second_name"} };
    vk_call_api(gc, "users.get", params, data->access_token(), [=](const picojson::value& result) {
        if (!result.is<picojson::array>()) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().c_str());
            return;
        }

        picojson::array users = result.get<picojson::array>();
        if (users.size() != 1) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().c_str());
            return;
        }

        if (!users[0].is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().c_str());
            return;
        }

        picojson::object user_fields = users[0].get<picojson::object>();
        if (!field_is_present<string>(user_fields, "first_name") || !field_is_present<string>(user_fields, "last_name")) {
            purple_debug_error("prpl-vkcom", "Wrong type returned as users.get call result: %s\n",
                               result.serialize().c_str());
            return;
        }
        string first_name = user_fields.at("first_name").get<string>();
        string last_name = user_fields.at("last_name").get<string>();

        success_cb(first_name + " " + last_name);
    }, nullptr);
}
