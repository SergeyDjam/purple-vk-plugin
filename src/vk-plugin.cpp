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
#include "vk-message-send.h"


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

    type = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, "offline", nullptr, TRUE, TRUE, FALSE);
    types = g_list_prepend(types, type);

    return g_list_reverse(types);
}

// Returns text, which is shown under each buddy list item.
char* vk_status_text(PurpleBuddy* buddy)
{
    PurplePresence* presence = purple_buddy_get_presence(buddy);
    if (purple_presence_is_online(presence)) {
        VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);
        if (!data)
            return nullptr;
        if (data->activity.empty())
            return nullptr;
        return g_markup_escape_text(data->activity.data(), -1);
    } else {
        return nullptr;
    }
}

// Returns text, which is shown when mouse hovers over list.
void vk_tooltip_text(PurpleBuddy* buddy, PurpleNotifyUserInfo* info, gboolean)
{
    VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);
    if (!data)
        return;

    if (!data->activity.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Status", data->activity.data());
    if (data->is_mobile)
        purple_notify_user_info_add_pair_plaintext(info, "Uses mobile client", nullptr);
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
            get_buddy_full_name(gc, data->uid(), [=](const string& full_name) {
                purple_account_set_alias(account, full_name.data());
            });
        }

        // Start Long Poll event processing. Buddy list and unread messages will be retrieved there.
        start_long_poll(gc);

        // Add updating buddy list every 15 minutes. If we do not update regularily, we might miss
        // updates to buddy status text, buddy icon or other information. Do not update buddy presence,
        // as it is now managed by longpoll.
        timeout_add(gc, 15 * 60 * 1000, [=] {
            update_buddy_list(gc, false);
            return true;
        });
    }, nullptr);
}

void vk_close(PurpleConnection* gc)
{
    VkConnData* data = get_conn_data(gc);
    data->set_closing();

    timeout_remove_all(gc);
    purple_request_close_with_handle(gc);
    purple_http_conn_cancel_all(gc);

    purple_connection_set_protocol_data(gc, nullptr);
    delete data;
}

int vk_send_im(PurpleConnection* gc, const char* to, const char* message, PurpleMessageFlags)
{
    return send_im_message(gc, uid_from_buddy_name(to), message);
}

unsigned int vk_send_typing(PurpleConnection* gc, const char* name, PurpleTypingState state)
{
    if (state != PURPLE_TYPING)
        return 0;
    return send_typing_notification(gc, uid_from_buddy_name(name));
}

// Returns link to vk.com user page
string get_user_page(PurpleBuddy* buddy, const VkBuddyData* data)
{
    if (data && !data->domain.empty())
        return str_format("http://vk.com/%s", data->domain.data());
    else
        return str_format("http://vk.com/%s", purple_buddy_get_name(buddy));
}

// Called when user chooses "Get Info".
void vk_get_info(PurpleConnection* gc, const char* username)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    PurpleBuddy* buddy = purple_find_buddy(account, username);
    VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);

    PurpleNotifyUserInfo* info = purple_notify_user_info_new();
    purple_notify_user_info_add_pair(info, "Page", get_user_page(buddy, data).data());

    if (!data) {
        purple_notify_userinfo(gc, username, info, nullptr, nullptr);
        return;
    }

    purple_notify_user_info_add_section_break(info);
    purple_notify_user_info_add_pair_plaintext(info, "Name", purple_buddy_get_contact_alias(buddy));

    if (!data->bdate.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Birthdate", data->bdate.data());
    if (!data->education.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Education", data->education.data());
    if (!data->mobile_phone.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Mobile phone", data->mobile_phone.data());
    if (!data->activity.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Status", data->activity.data());

    purple_notify_userinfo(gc, username, info, nullptr, nullptr);
}

void vk_buddy_free(PurpleBuddy* buddy)
{
    VkBuddyData* data = (VkBuddyData*)purple_buddy_get_protocol_data(buddy);
    delete data;
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
    nullptr, /* set_status */
    nullptr, /* set_idle */
    nullptr, /* change_passwd */
    nullptr, /* add_buddy */
    nullptr, /* add_buddies */
    nullptr, /* remove_buddy */
    nullptr, /* remove_buddies */
    nullptr, /* add_permit */
    nullptr, /* add_deny */
    nullptr, /* rem_permit */
    nullptr, /* rem_deny */
    nullptr, /* set_permit_deny */
    nullptr, //    waprpl_chat_join, /* join_chat */
    nullptr, /* reject_chat */
    nullptr, //    waprpl_get_chat_name, /* get_chat_name */
    nullptr, //    waprpl_chat_invite, /* chat_invite */
    nullptr, /* chat_leave */
    nullptr, /* chat_whisper */
    nullptr, //    waprpl_send_chat, /* chat_send */
    nullptr, /* keepalive */
    nullptr, /* register_user */
    nullptr, /* get_cb_info */
    nullptr, /* get_cb_away */
    nullptr, /* alias_buddy */
    nullptr, /* group_buddy */
    nullptr, /* rename_group */
    vk_buddy_free, /* buddy_free */
    nullptr, /* convo_closed */
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
    nullptr, /* add_buddy_with_invite */
    nullptr /* add_buddies_with_invite */
};

gboolean load_plugin(PurplePlugin*)
{
    purple_http_init();
    return true;
}

gboolean unload_plugin(PurplePlugin*)
{
    destroy_keepalive_pool();
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
    (char*)"0.3", /* version */
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
    // We destroy a bunch of HTTP connections on exit, so we have to add dependency on ssl, otherwise ssl_close
    // will throw sigsegv.
    info.dependencies = g_list_append(info.dependencies, g_strdup("core-ssl"));
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
