#include <accountopt.h>
#include <debug.h>
#include <prpl.h>
#include <request.h>
#include <version.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-chat.h"
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

// Returns text, which is shown under each buddy list item.
char* vk_status_text(PurpleBuddy* buddy)
{
    PurplePresence* presence = purple_buddy_get_presence(buddy);
    if (purple_presence_is_online(presence)) {
        VkUserInfo* user_info = get_user_info(buddy);
        if (!user_info)
            return nullptr;
        if (user_info->activity.empty())
            return nullptr;
        return g_markup_escape_text(user_info->activity.data(), -1);
    } else {
        return nullptr;
    }
}

#if !PURPLE_VERSION_CHECK(2, 8, 0)
void purple_notify_user_info_add_pair_plaintext(PurpleNotifyUserInfo *user_info, const char *label, const char *value)
{
    gchar *escaped;
    escaped = g_markup_escape_text(value, -1);
    purple_notify_user_info_add_pair(user_info, label, escaped);
    g_free(escaped);
}
#endif

// Returns text, which is shown when mouse hovers over list.
void vk_tooltip_text(PurpleBuddy* buddy, PurpleNotifyUserInfo* info, gboolean)
{
    VkUserInfo* user_info = get_user_info(buddy);
    if (!user_info) {
        purple_notify_user_info_add_pair_plaintext(info, "Updating data...", nullptr);
        return;
    }

    if (!user_info->domain.empty())
        purple_notify_user_info_add_pair_plaintext(info, "Nickname", user_info->domain.data());

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
        vkcom_debug_info("Active conversation changed\n");

        // Pidgin sends this signal before the conversation becomes focused, so we have to run
        // a bit later.
        timeout_add(gc, 0, [=] {
            mark_deferred_messages_as_read(gc, false);
            return false;
        });
    }
}

//GList* vk_chat_join_info(PurpleConnection*)
//{
//    proto_chat_entry* pce = g_new0(proto_chat_entry, 1);
//    pce->label = "_Title:";
//    pce->identifier = "title";
//    pce->required = true;
//    return g_list_append(nullptr, pce);
//}

//GHashTable* vk_chat_info_defaults(PurpleConnection*, const char* chat_name)
//{
//    GHashTable *defaults = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, g_free);

//    if (chat_name != NULL)
//        g_hash_table_insert(defaults, (void*)"id", g_strdup(chat_name));

//    return defaults;
//}


// Sets option with given name to value if not already set.
void convert_option_bool(PurpleAccount* account, const char* name, bool previous_value)
{
    bool value = purple_account_get_bool(account, name, previous_value);
    purple_account_set_bool(account, name, value);
}

// Convert old options to new.
void convert_options(PurpleAccount* account)
{
    // The change may seem strange, but the old option name was really inappropriate.
    bool mark_as_read_inactive_tab = purple_account_get_bool(account, "mark_as_read_instantaneous", false);
    convert_option_bool(account, "mark_as_read_inactive_tab", mark_as_read_inactive_tab);
}

void vk_login(PurpleAccount* account)
{
    vkcom_debug_info("Opening connection\n");

    convert_options(account);

    PurpleConnection* gc = purple_account_get_connection(account);

    gc->flags = PurpleConnectionFlags(gc->flags | PURPLE_CONNECTION_NO_BGCOLOR | PURPLE_CONNECTION_NO_FONTSIZE);

    const char* email = purple_account_get_username(account);
    const char* password = purple_account_get_password(account);
    VkData* gc_data = new VkData(gc, email, password);
    purple_connection_set_protocol_data(gc, gc_data);

    gc_data->authenticate([=] {
        // Set account alias to full user name if alias not set previously.
        PurpleAccount* account = purple_connection_get_account(gc);
        const char* alias = purple_account_get_alias(account);
        if (!alias || !alias[0]) {
            set_account_alias(gc);
        }

        // Remember current aliases and groups of buddies and chats to check whether user has modified them later.
        check_blist_on_login(gc);

        // Start Long Poll event processing. Buddy list and unread messages will be retrieved there.
        start_long_poll(gc);

        // Add updating users and chats information every 15 minutes. If we do not update regularily, we might miss
        // updates to buddy status text, buddy icon or other information. First time user and chat infos are
        // updated when longpoll starts.
        timeout_add(gc, 15 * 60 * 1000, [=] {
            update_user_chat_infos(gc);
            return true;
        });

        // Longpoll only notifies about status of friends. If we have conversations open with non-friends,
        // we update their status every minute.
        timeout_add(gc, 60 * 1000, [=] {
            update_open_conv_presence(gc);
            return true;
        });

        // Update that we are online every 15 minutes.
        update_status(gc);
        timeout_add(gc, 15 * 60 * 1000, [=] {
            update_status(gc);
            return true;
        });

        purple_signal_connect(purple_conversations_get_handle(), "conversation-updated", gc,
                              PURPLE_CALLBACK(conversation_updated), gc);
    }, [=] {
    });
}

void vk_close(PurpleConnection* gc)
{
    vkcom_debug_info("Closing connection\n");

    purple_signal_disconnect(purple_conversations_get_handle(), "conversation-updated", gc,
                          PURPLE_CALLBACK(conversation_updated));

    set_offline(gc);
    // Let's sleep 250 msec, so that setOffline executes successfully. Yes, it is ugly, but
    // we cannot defer destruction of PurpleConnection and doing the "right way" is such a bother.
    g_usleep(250000);

    VkData& data = get_data(gc);
    data.set_closing();

    purple_request_close_with_handle(gc);
    // TODO: Pidgin crashes if we cancel more than one http_get request in here. Either do not
    // run parallel requests when fetching buddy icons or do something with timeouts.
    purple_http_conn_cancel_all(gc);

    check_blist_on_logout(gc);

    purple_connection_set_protocol_data(gc, nullptr);
    delete &data;
}

int vk_send_im(PurpleConnection* gc, const char* who, const char* message, PurpleMessageFlags)
{
    uint64 user_id = user_id_from_name(who);
    if (user_id == 0) {
        vkcom_debug_info("Trying to send message to unknown user %s\n", who);
        return 0;
    }
    mark_deferred_messages_as_read(gc, true);
    return send_im_message(gc, user_id, message);
}

unsigned int vk_send_typing(PurpleConnection* gc, const char* who, PurpleTypingState state)
{
    if (state != PURPLE_TYPING)
        return 0;

    uint64 user_id = user_id_from_name(who);
    if (user_id == 0) {
        vkcom_debug_info("Trying to send message to unknown user %s\n", who);
        return 0;
    }
    mark_deferred_messages_as_read(gc, true);
    return send_typing_notification(gc, user_id);
}

// Returns link to vk.com user page
string get_user_page(const char* who, const VkUserInfo* info)
{
    if (info && !info->domain.empty())
        return str_format("https://vk.com/%s", info->domain.data());
    else
        return str_format("https://vk.com/%s", who);
}

// Called when user chooses "Get Info".
void vk_get_info(PurpleConnection* gc, const char* who)
{
    vkcom_debug_info("Requesting user info for %s\n", who);

    PurpleNotifyUserInfo* info = purple_notify_user_info_new();
    uint64 user_id = user_id_from_name(who);
    if (user_id == 0) {
        purple_notify_user_info_add_pair(info, "User is not a Vk.com user", nullptr);
        purple_notify_userinfo(gc, who, info, nullptr, nullptr);
        return;
    }

    VkUserInfo* user_info = get_user_info(gc, user_id);
    purple_notify_user_info_add_pair(info, "Page", get_user_page(who, user_info).data());
    if (!user_info) {
        purple_notify_user_info_add_pair(info, "Updating data...", nullptr);
        purple_notify_userinfo(gc, who, info, nullptr, nullptr);
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
        purple_notify_user_info_add_pair_plaintext(info, "Name", user_info->real_name.data());

        if (!user_info->bdate.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Birthdate", user_info->bdate.data());
        if (!user_info->education.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Education", user_info->education.data());
        if (!user_info->mobile_phone.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Mobile phone", user_info->mobile_phone.data());
        if (!user_info->activity.empty())
            purple_notify_user_info_add_pair_plaintext(info, "Status", user_info->activity.data());
        purple_notify_userinfo(gc, who, info, nullptr, nullptr);
    });
}

// Called when user changes the status.
void vk_set_status(PurpleAccount* account, PurpleStatus* status)
{
    // We consider only changing to Available to be a "user activity".
    PurpleStatusPrimitive primitive_status = purple_status_type_get_primitive(purple_status_get_type(status));
    if (primitive_status == PURPLE_STATUS_AVAILABLE)
        mark_deferred_messages_as_read(purple_account_get_connection(account), true);
    update_status(purple_account_get_connection(account));
}

void vk_add_buddy_with_invite(PurpleConnection* gc, PurpleBuddy* buddy, PurpleGroup* group, const char*);

// We need this method in order to be compatible with Pidgin < 2.8
void vk_add_buddy(PurpleConnection* gc, PurpleBuddy* buddy, PurpleGroup* group)
{
    vk_add_buddy_with_invite(gc, buddy, group, "");
}

void vk_remove_buddy(PurpleConnection* gc, PurpleBuddy* buddy, PurpleGroup*)
{
    const char* name = purple_buddy_get_name(buddy);
    vkcom_debug_info("Manually removing buddy %s\n", name);

    uint64 user_id = user_id_from_name(name);
    if (user_id == 0)
        return;

    get_data(gc).set_manually_removed_buddy(user_id);
}

void vk_chat_join(PurpleConnection* gc, GHashTable* components)
{
    const char* chat_name = (const char*)g_hash_table_lookup(components, "id");
    if (chat_name) {
        vkcom_debug_info("Joining %s\n", chat_name);

        uint64 chat_id = chat_id_from_name(chat_name);
        open_chat_conv(gc, chat_id, [=] {
            // Pidgin does not activate newly created conversations, let's do it ourselves.
            PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
            purple_conversation_present(conv);
        });
    } else {
    }
}

char* vk_get_chat_name(GHashTable* components)
{
    const char* chat_name = (const char*)g_hash_table_lookup(components, "id");

    // This function gets called after "Join chat", before the chat is actually joined.
    if (chat_name)
        return g_strdup(chat_name);
    else
        return g_strdup("CHAT NOT CREATED");
}

//void vk_chat_invite(PurpleConnection* gc, int conv_id, const char*, const char* who)
//{
//}

void vk_chat_leave(PurpleConnection* gc, int id)
{
    uint64 chat_id = conv_id_to_chat_id(gc, id);
    if (chat_id == 0) {
        vkcom_debug_info("Trying to leave chat %d\n", id);
        return;
    }

    vkcom_debug_error("Leaving chat%" PRIu64 "\n", chat_id);
    remove_conv_id(gc, id);
    remove_chat_if_needed(gc, chat_id);
}

int vk_chat_send(PurpleConnection* gc, int id, const char* message, PurpleMessageFlags)
{
    uint64 chat_id = conv_id_to_chat_id(gc, id);
    if (chat_id == 0) {
        vkcom_debug_info("Trying to send message to unknown chat %d\n", id);
        return 0;
    }

    mark_deferred_messages_as_read(gc, true);

    // Pidgin for some reason does not write outgoing messages when writing to the chat,
    // so we have to do it oruselves.
    PurpleConversation* conv = purple_find_chat(gc, id);
    string from = get_self_chat_display_name(gc);
    purple_conv_chat_write(PURPLE_CONV_CHAT(conv), from.data(), message, PURPLE_MESSAGE_SEND, time(nullptr));

    return send_chat_message(gc, chat_id, message);
}

void vk_alias_buddy(PurpleConnection* gc, const char *, const char *)
{
    // Recheck all open chats and update because they could contain the aliased buddy.
    update_all_open_chat_convs(gc);
}

// A dummy "rename group" is required so that libpurple client does not remove and re-add all buddies
// in the process of mere renaming of a group.
void vk_rename_group(PurpleConnection*, const char*,  PurpleGroup*, GList*)
{
}

// Conversation is closed, we may want to remove buddy from buddy list if it has been added there temporarily.
void vk_convo_closed(PurpleConnection* gc, const char* who)
{
    vkcom_debug_info("Conversation with %s closed\n", who);

    uint64 user_id = user_id_from_name(who);
    if (user_id == 0)
        return;

    // We must call remove_buddy_if_needed later, otherwise the conversation is still open when this
    // function is closed and buddy is not removed.
    timeout_add(gc, 0, [=] {
        remove_buddy_if_needed(gc, user_id);
        return false;
    });
}

PurpleChat* vk_find_blist_chat(PurpleAccount* account, const char* name)
{
    for (PurpleChat* chat: find_all_purple_chats(account)) {
        const char* chat_name = (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "id");
        if (g_str_equal(chat_name, name))
            return chat;
    }

    vkcom_debug_error("Unable to find chat with name %s\n", name);
    return nullptr;
}

char* vk_get_cb_real_name(PurpleConnection* gc, int id, const char* who)
{
    uint64 user_id = find_user_id_from_conv(gc, id, who);
    if (user_id == 0) {
        const char* self_alias = purple_account_get_alias(purple_connection_get_account(gc));
        if (g_str_equal(who, self_alias))
            user_id = get_data(gc).self_user_id();
    }

    if (user_id != 0) {
        // We will probably open the chat with the user, so let's add it to the buddy list.
        add_buddy_if_needed(gc, user_id);
        return g_strdup(user_name_from_id(user_id).data());
    } else {
        return nullptr;
    }
}

void vk_set_chat_topic(PurpleConnection*, int, const char*)
{
}

gboolean vk_can_receive_file(PurpleConnection*, const char*)
{
    return true;
}

PurpleXfer* vk_new_xfer(PurpleConnection* gc, const char* who)
{
    uint64 user_id = user_id_from_name(who);
    if (user_id == 0) {
        vkcom_debug_info("Trying to send file to unknown user %s\n", who);
        return nullptr;
    }
    return new_xfer(gc, user_id);
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
    vkcom_debug_info("Manually adding buddy\n");

    // Get only the latest part of URL if we've been supplied with it.
    string buddy_name = str_rsplit(purple_buddy_get_name(buddy), '/');

    // Store alias and group name, as buddy will be removed momentarily and readded later.
    string alias = purple_buddy_get_alias(buddy);
    if (alias == purple_buddy_get_name(buddy))
        alias.clear();
    string group_name = purple_group_get_name(group);

    resolve_screen_name(gc, buddy_name.data(), [=](const string& type, uint64 user_id) {
        purple_blist_remove_buddy(buddy);

        if (type != "user") {
            string title = str_format("Unable to find user %s", buddy_name.data());
            const char* message = "User name should be either idXXXXXX or nickname"
                    " (i.e. the last part of https://vk.com/nickname)";
            purple_notify_error(gc, title.data(), title.data(), message);
            return;
        }

        get_data(gc).set_manually_added_buddy(user_id);

        add_buddy_if_needed(gc, user_id, [=] {
            PurpleBuddy* buddy = purple_find_buddy(purple_connection_get_account(gc),
                                                   user_name_from_id(user_id).data());
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
    // OPT_PROTO_UNIQUE_CHATNAME prevents libpurple messing with names of chat users (we use
    // real names, not idXXX for them).
    PurpleProtocolOptions(OPT_PROTO_IM_IMAGE | OPT_PROTO_UNIQUE_CHATNAME), /* options */
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
    nullptr, //    vk_chat_join_info, /* chat_info */
    nullptr, //    vk_chat_info_defaults, /* chat_info_defaults */
    vk_login, /* login */
    vk_close, /* close */
    vk_send_im, /* send_im */
    nullptr, /* set_info */
    vk_send_typing, /* send_typing */
    vk_get_info, /* get_info */
    vk_set_status, /* set_status */
    nullptr, /* set_idle */
    nullptr, /* change_passwd */
    vk_add_buddy, /* add_buddy */
    nullptr, /* add_buddies */
    vk_remove_buddy, /* remove_buddy */
    nullptr, /* remove_buddies */
    nullptr, /* add_permit */
    nullptr, /* add_deny */
    nullptr, /* rem_permit */
    nullptr, /* rem_deny */
    nullptr, /* set_permit_deny */
    vk_chat_join, /* join_chat */
    nullptr, /* reject_chat */
    vk_get_chat_name, /* get_chat_name */
    nullptr, //    vk_chat_invite, /* chat_invite */
    vk_chat_leave, /* chat_leave */
    nullptr, /* chat_whisper */
    vk_chat_send, /* chat_send */
    nullptr, /* keepalive */
    nullptr, /* register_user */
    nullptr, /* get_cb_info */
    nullptr, /* get_cb_away */
    vk_alias_buddy, /* alias_buddy */
    nullptr, /* group_buddy */
    vk_rename_group, /* rename_group */
    nullptr, /* buddy_free */
    vk_convo_closed, /* convo_closed */
    purple_normalize_nocase, /* normalize */
    nullptr, /* set_buddy_icon */
    nullptr, /* remove_group */
    vk_get_cb_real_name, /* get_cb_real_name */
    nullptr, //    vk_set_chat_topic, /* set_chat_topic */
    vk_find_blist_chat, /* find_blist_chat */
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
#if PURPLE_VERSION_CHECK(2, 8, 0)
    vk_add_buddy_with_invite, /* add_buddy_with_invite */
    nullptr /* add_buddies_with_invite */
#endif // PURPLE_VERSION_CHECK(2, 8, 0)
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
    (char*)"0.9", /* version */
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
    option = purple_account_option_bool_new("Show only friends in buddy list", "only_friends_in_blist", true);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Show chats in buddy list", "chats_in_blist", true);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Do not mark messages as read when away", "mark_as_read_online_only", true);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Mark messages as read even if in inactive tab", "mark_as_read_inactive_tab", false);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_bool_new("Imitate using mobile client", "imitate_mobile_client", false);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_string_new("Group for buddies", "blist_default_group", "");
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

    option = purple_account_option_string_new("Group for chats", "blist_chat_group", "");
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
