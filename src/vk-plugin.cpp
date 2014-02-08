#include <accountopt.h>
#include <debug.h>
#include <prpl.h>
#include <request.h>
#include <version.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-filexfer.h"
#include "vk-longpoll.h"
#include "vk-message-recv.h"
#include "vk-message-send.h"
#include "vk-status.h"
#include "vk-utils.h"


const char* vk_list_icon(PurpleAccount*, PurpleBuddy*)
{
    return "vkontakte";
}

GList* vk_status_types(PurpleAccount*)
{
    GList* types = nullptr;
    PurpleStatusType* type;

    type = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "online", nullptr, TRUE, TRUE, FALSE);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_AWAY, "away", nullptr, TRUE, TRUE, FALSE);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_INVISIBLE, "invisible", nullptr, TRUE, TRUE, FALSE);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, "offline", nullptr, TRUE, TRUE, FALSE);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_full(PURPLE_STATUS_MOBILE, "mobile", nullptr, FALSE, FALSE, FALSE);
    types = g_list_prepend(types, type);

    return g_list_reverse(types);
}

namespace
{

} // End of anonymous namespace

// Returns text, which is shown under each buddy list item.
char* vk_status_text(PurpleBuddy* buddy)
{
    PurplePresence* presence = purple_buddy_get_presence(buddy);
    if (purple_presence_is_online(presence)) {
        VkUserInfo* user_info = get_user_info_for_buddy(buddy);
        if (!user_info)
            return nullptr;
        if (user_info->activity.empty())
            return nullptr;
        return g_markup_escape_text(user_info->activity.data(), -1);
    } else {
        return nullptr;
    }
}

// Returns text, which is shown when mouse hovers over list.
void vk_tooltip_text(PurpleBuddy* buddy, PurpleNotifyUserInfo* info, gboolean)
{
    VkUserInfo* user_info = get_user_info_for_buddy(buddy);
    if (!user_info)
        return;

    if (!user_info->activity.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Status", user_info->activity.data());
    if (user_info->online_mobile)
        purple_notify_user_info_add_pair_plaintext(info, "Uses mobile client", nullptr);
}

// Signal handler for conversation-updated signal, which is received when user changes active
// conversation.
void conversation_updated(PurpleConversation* conv, PurpleConvUpdateType type, gpointer data)
{
    PurpleConnection* gc = (PurpleConnection*)data;

    // This is not our conversation.
    if (gc != purple_conversation_get_gc(conv))
        return;

    if (type == PURPLE_CONV_UPDATE_UNSEEN) {
        // Pidgin sends this signal before the conversation becomes focused, so we have to run
        // a bit later.
        timeout_add(gc, 0, [=] {
            mark_deferred_messages_as_read(gc, false);
            return false;
        });
    }
}

void vk_login(PurpleAccount* acct)
{
    PurpleConnection* gc = purple_account_get_connection(acct);

    gc->flags = PurpleConnectionFlags(gc->flags | PURPLE_CONNECTION_NO_BGCOLOR | PURPLE_CONNECTION_NO_FONTSIZE);

    const char* email = purple_account_get_username(acct);
    const char* password = purple_account_get_password(acct);
    VkConnData* data = new VkConnData(gc, email, password);
    purple_connection_set_protocol_data(gc, data);

    data->authenticate([=] {
        // Set account alias to full user name if alias not set previously.
        PurpleAccount* account = purple_connection_get_account(gc);
        const char* alias = purple_account_get_alias(account);
        if (!alias || !alias[0]) {
            set_account_alias(gc, data->uid());
        }

        // Start Long Poll event processing. Buddy list and unread messages will be retrieved there.
        start_long_poll(gc);

        // Add updating buddy list every 15 minutes. If we do not update regularily, we might miss
        // updates to buddy status text, buddy icon or other information. Do not update buddy presence,
        // as it is now managed by longpoll.
        timeout_add(gc, 15 * 60 * 1000, [=] {
            update_buddies(gc, false);
            return true;
        });

        // Longpoll only notifies about status of friends. If we have conversations open with non-friends,
        // we update their status every minute.
        timeout_add(gc, 60 * 1000, [=] {
            update_open_conversation_presence(gc);
            return true;
        });

        vk_update_status(gc);
        // Update that we are online every 15 minutes.
        timeout_add(gc, 15 * 60 * 1000, [=] {
            vk_update_status(gc);
            return true;
        });

        purple_signal_connect(purple_conversations_get_handle(), "conversation-updated", gc,
                              PURPLE_CALLBACK(conversation_updated), gc);
    }, nullptr);
}

void vk_close(PurpleConnection* gc)
{
    purple_signal_disconnect(purple_conversations_get_handle(), "conversation-updated", gc,
                          PURPLE_CALLBACK(conversation_updated));

    vk_set_offline(gc);
    // Let's sleep 250 msec, so that setOffline executes successfully. Yes, it is ugly, but
    // we cannot defer destruction of PurpleConnection and doing the "right way" is such a bother.
    g_usleep(250000);

    VkConnData* data = get_conn_data(gc);
    data->set_closing();

    timeout_remove_all(gc);
    purple_request_close_with_handle(gc);
    purple_http_conn_cancel_all(gc);
    destroy_keepalive_pool(gc);

    purple_connection_set_protocol_data(gc, nullptr);
    delete data;
}

int vk_send_im(PurpleConnection* gc, const char* to, const char* message, PurpleMessageFlags)
{
    mark_deferred_messages_as_read(gc, true);
    return send_im_message(gc, uid_from_buddy_name(to), message);
}

unsigned int vk_send_typing(PurpleConnection* gc, const char* name, PurpleTypingState state)
{
    if (state != PURPLE_TYPING)
        return 0;
    mark_deferred_messages_as_read(gc, true);
    return send_typing_notification(gc, uid_from_buddy_name(name));
}

// Returns link to vk.com user page
string get_user_page(const char* name, const VkUserInfo* info)
{
    if (info && !info->domain.empty())
        return str_format("http://vk.com/%s", info->domain.data());
    else
        return str_format("http://vk.com/%s", name);
}

// Called when user chooses "Get Info".
void vk_get_info(PurpleConnection* gc, const char* username)
{
    VkUserInfo* user_info = get_user_info_for_buddy(gc, username);

    PurpleNotifyUserInfo* info = purple_notify_user_info_new();
    purple_notify_user_info_add_pair(info, "Page", get_user_page(username, user_info).data());

    if (!user_info) {
        purple_notify_userinfo(gc, username, info, nullptr, nullptr);
        return;
    }

    http_get(gc, user_info->photo_max.data(), [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (purple_http_response_is_successful(response)) {
            size_t size;
            const char* data = purple_http_response_get_data(response, &size);
            int img_id = purple_imgstore_add_with_id(g_memdup(data, size), size, nullptr);
            if (img_id != 0) {
                string img = str_format("<img id='%d'>", img_id);
                purple_notify_user_info_add_pair(info, nullptr, img.data());
            }
        }

        purple_notify_user_info_add_section_break(info);
        purple_notify_user_info_add_pair_plaintext(info, "Name", user_info->name.data());

        if (!user_info->bdate.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Birthdate", user_info->bdate.data());
        if (!user_info->education.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Education", user_info->education.data());
        if (!user_info->mobile_phone.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Mobile phone", user_info->mobile_phone.data());
        if (!user_info->activity.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Status", user_info->activity.data());
        purple_notify_userinfo(gc, username, info, nullptr, nullptr);
    });
}

// Called when user changes the status.
void vk_set_status(PurpleAccount* account, PurpleStatus* status)
{
    // We consider only changing to Available to be a "user activity".
    PurpleStatusPrimitive primitive_status = purple_status_type_get_primitive(purple_status_get_type(status));
    if (primitive_status == PURPLE_STATUS_AVAILABLE)
        mark_deferred_messages_as_read(purple_account_get_connection(account), true);
    vk_update_status(purple_account_get_connection(account));
}

void vk_remove_buddy(PurpleConnection* gc, PurpleBuddy* buddy, PurpleGroup*)
{
    uint64 user_id = uid_from_buddy_name(purple_buddy_get_name(buddy));
    if (user_id == 0)
        return;

    VkConnData* conn_data = get_conn_data(gc);
    if (contains_key(conn_data->manually_added_buddies, user_id))
        conn_data->manually_added_buddies.erase(user_id);
    else
        conn_data->manually_removed_buddies.insert(user_id);
}

char* vk_get_chat_name(GHashTable* data)
{
    return g_strdup((const char*)g_hash_table_lookup(data, "title"));
}

int vk_chat_send(PurpleConnection* gc, int id, const char* message, PurpleMessageFlags)
{
    return send_chat_message(gc, id, message);
}

// We do not store alias on server, but we can set the flag, so that the alias will not be overwritten
// on next update of the buddy list.
void vk_alias_buddy(PurpleConnection* gc, const char* who, const char*)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, who);
    if (!buddy)
        return;

    purple_blist_node_set_bool(&buddy->node, "custom-alias", true);
}

void vk_group_buddy(PurpleConnection* gc, const char* who, const char*, const char* new_group)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, who);
    if (!buddy)
        return;

    const char* default_group = purple_account_get_string(purple_connection_get_account(gc),
                                                          "blist_default_group", "");
    if (!g_str_equal(new_group, default_group)) {
        purple_blist_node_set_bool(&buddy->node, "custom-group", true);
    } else {
        // Set "custom-group" only if it has been set before. This happens when
        // the user changes default group and all buddies from the old default group are
        // moved to new one.
        if (purple_blist_node_get_bool(&buddy->node, "custom-group"))
            purple_blist_node_set_bool(&buddy->node, "custom-group", false);
    }
}

// A dummy "rename group" is required so that libpurple client does not remove and re-add all buddies
// in the process of mere renaming of a group.
void vk_rename_group(PurpleConnection*, const char*,  PurpleGroup*, GList*)
{
}

// Conversation is closed, we may want to remove buddy from buddy list if it has been added there temporarily.
void vk_convo_closed(PurpleConnection* gc, const char* who)
{
    uint64 uid = uid_from_buddy_name(who);
    // We must call remove_buddy_if_needed later, otherwise the conversation is still open when this
    // function is closed and buddy is not removed.
    timeout_add(gc, 0, [=] {
        remove_buddy_if_needed(gc, uid);
        return false;
    });
}

gboolean vk_can_receive_file(PurpleConnection*, const char*)
{
    return true;
}

PurpleXfer* vk_new_xfer(PurpleConnection* gc, const char* who)
{
    return new_xfer(gc, uid_from_buddy_name(who));
}

void vk_send_file(PurpleConnection* gc, const char* who, const char* filename)
{
    PurpleXfer* xfer = vk_new_xfer(gc, who);
    if (filename)
        purple_xfer_request_accepted(xfer, filename);
    else
        purple_xfer_request(xfer);

    mark_deferred_messages_as_read(gc, true);
}

gboolean vk_offline_message(const PurpleBuddy*)
{
    return true;
}

GHashTable* vk_get_account_text_table(PurpleAccount*)
{
    GHashTable* table = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(table, g_strdup("login_label"), g_strdup("E-mail or telephone"));
    return table;
}

void vk_add_buddy_with_invite(PurpleConnection* gc, PurpleBuddy* buddy, PurpleGroup* group, const char*)
{
    // Get only the latest part of URL if we've been supplied with it.
    string buddy_name = str_rsplit(purple_buddy_get_name(buddy), '/');

    // Store alias and group name, as buddy will be removed momentarily and readded later.
    string alias = purple_buddy_get_alias(buddy);
    string group_name = purple_group_get_name(group);

    resolve_screen_name(gc, buddy_name.data(), [=](const string& type, uint64 user_id) {
        purple_blist_remove_buddy(buddy);

        if (type != "user") {
            string title = str_format("Unable to find user %s", buddy_name.data());
            const char* message = "User name should be either idXXXXXX or nickname"
                    " (i.e. the last part of http://vk.com/nickname)";
            purple_notify_error(gc, title.data(), title.data(), message);
            return;
        }

        VkConnData* conn_data = get_conn_data(gc);
        if (contains_key(conn_data->manually_removed_buddies, user_id))
            conn_data->manually_removed_buddies.erase(user_id);
        else
            conn_data->manually_added_buddies.insert(user_id);

        add_buddy_if_needed(gc, user_id, [=] {
            PurpleBuddy* buddy = purple_find_buddy(purple_connection_get_account(gc),
                                                   buddy_name_from_uid(user_id).data());
            assert(buddy);

            if (!alias.empty()) {
                purple_blist_alias_buddy(buddy, alias.data());
                purple_blist_node_set_bool(&buddy->node, "custom-alias", true);
            }

            string default_group = purple_account_get_string(purple_connection_get_account(gc),
                                                             "blist_default_group", "");
            if (group_name != default_group) {
                PurpleGroup* new_group = purple_group_new(group_name.data());
                purple_blist_add_buddy(buddy, nullptr, new_group, nullptr);
                purple_blist_node_set_bool(&buddy->node, "custom-group", true);
            }
        });
    });
}

PurplePluginProtocolInfo prpl_info = {
    PurpleProtocolOptions(OPT_PROTO_IM_IMAGE), /* options */
    nullptr, /* user_splits */
    nullptr, /* protocol_options, initialized in waprpl_init() */
    { /* icon_spec, a PurpleBuddyIconSpec */
        (char*)"png,jpg", /* format */
        1, /* min_width */
        1, /* min_height */
        512, /* max_width */
        512, /* max_height */
        64000, /* max_filesize */
        PURPLE_ICON_SCALE_SEND, /* scale_rules */
    },
    vk_list_icon, /* list_icon */
    nullptr, /* list_emblem */
    vk_status_text, /* status_text */
    vk_tooltip_text, /* tooltip_text */
    vk_status_types, /* status_types */
    nullptr, /* blist_node_menu */
    nullptr, //    waprpl_chat_join_info, /* chat_info */
    nullptr, //    waprpl_chat_info_defaults, /* chat_info_defaults */
    vk_login, /* login */
    vk_close, /* close */
    vk_send_im, /* send_im */
    nullptr, /* set_info */
    vk_send_typing, /* send_typing */
    vk_get_info, /* get_info */
    vk_set_status, /* set_status */
    nullptr, /* set_idle */
    nullptr, /* change_passwd */
    nullptr, /* add_buddy */
    nullptr, /* add_buddies */
    vk_remove_buddy, /* remove_buddy */
    nullptr, /* remove_buddies */
    nullptr, /* add_permit */
    nullptr, /* add_deny */
    nullptr, /* rem_permit */
    nullptr, /* rem_deny */
    nullptr, /* set_permit_deny */
    nullptr, //    waprpl_chat_join, /* join_chat */
    nullptr, /* reject_chat */
    vk_get_chat_name, /* get_chat_name */
    nullptr, //    waprpl_chat_invite, /* chat_invite */
    nullptr, /* chat_leave */
    nullptr, /* chat_whisper */
    vk_chat_send, /* chat_send */
    nullptr, /* keepalive */
    nullptr, /* register_user */
    nullptr, /* get_cb_info */
    nullptr, /* get_cb_away */
    vk_alias_buddy, /* alias_buddy */
    vk_group_buddy, /* group_buddy */
    vk_rename_group, /* rename_group */
    nullptr, /* buddy_free */
    vk_convo_closed, /* convo_closed */
    purple_normalize_nocase, /* normalize */
    nullptr, /* set_buddy_icon */
    nullptr, /* remove_group */
    nullptr, /* get_cb_real_name */
    nullptr, /* set_chat_topic */
    nullptr, /* find_blist_chat */
    nullptr, /* roomlist_get_list */
    nullptr, /* roomlist_cancel */
    nullptr, /* roomlist_expand_category */
    vk_can_receive_file, /* can_receive_file */
    vk_send_file, /* send_file */
    vk_new_xfer, /* new_xfer */
    vk_offline_message, /* offline_message */
    nullptr, /* whiteboard_prpl_ops */
    nullptr, /* send_raw */
    nullptr, /* roomlist_room_serialize */
    nullptr, /* unregister_user */
    nullptr, /* send_attention */
    nullptr, /* get_attention_types */
    sizeof(PurplePluginProtocolInfo), /* struct_size */
    vk_get_account_text_table, /* get_account_text_table */
    nullptr, /* initiate_media */
    nullptr, /* get_media_caps */
    nullptr, /* get_moods */
    nullptr, /* set_public_alias */
    nullptr, /* get_public_alias */
    vk_add_buddy_with_invite, /* add_buddy_with_invite */
    nullptr /* add_buddies_with_invite */
};

gboolean load_plugin(PurplePlugin*)
{
    purple_http_init();
    return true;
}

gboolean unload_plugin(PurplePlugin*)
{
    return true;
}

PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC, /* magic */
    PURPLE_MAJOR_VERSION, /* major_version */
    PURPLE_MINOR_VERSION, /* minor_version */
    PURPLE_PLUGIN_PROTOCOL, /* type */
    nullptr, /* ui_requirement */
    0, /* flags */
    nullptr, /* dependencies */
    PURPLE_PRIORITY_DEFAULT, /* priority */
    (char*)"prpl-vkcom", /* id */
    (char*)"Vk.com", /* name */
    (char*)"0.5", /* version */
    (char*)"Vk.com chat protocol", /* summary */
    (char*)"Vk.com chat protocol", /* description */
    (char*)"Oleg Andreev (olegoandreev@yandex.ru)", /* author */
    (char*)"https://bitbucket.org/olegoandreev/purple-vk-plugin", /* homepage */
    load_plugin, /* load */
    unload_plugin, /* unload */
    nullptr, /* destroy */
    nullptr, /* ui_info */
    &prpl_info, /* extra_info */
    nullptr, /* prefs_info */
    nullptr, /* actions */
    nullptr, /* reserved1 */
    nullptr, /* reserved2 */
    nullptr, /* reserved3 */
    nullptr, /* reserved4 */
};

void vkcom_prpl_init(PurplePlugin*)
{
    // Options, listed on "Advanced" page when creating or modifying account.
    PurpleAccountOption *option;
    option = purple_account_option_bool_new("Show only friends in buddy list", "only_friends_in_blist", false);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Show chats in buddy list", "chats_in_blist", true);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Do not mark messages as read when away", "mark_as_read_online_only", true);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Always mark messages as read on receiving", "mark_as_read_instantaneous", false);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Imitate using mobile client", "imitate_mobile_client", false);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_string_new("Group for buddies", "blist_default_group", "");
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
}

extern "C"
{

// For some reason G_MODULE_EXPORT does not set default visibility.
#ifndef G_PLATFORM_WIN32

#undef G_MODULE_EXPORT
#define G_MODULE_EXPORT __attribute__ ((visibility ("default")))

#endif // G_PLATFORM_WIN32

PURPLE_INIT_PLUGIN(vkcom, vkcom_prpl_init, info)

}
