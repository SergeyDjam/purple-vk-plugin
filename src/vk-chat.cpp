#include <debug.h>

#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-chat.h"

int chat_id_to_conv_id(PurpleConnection* gc, uint64 chat_id)
{
    for (const pair<int, uint64>& it: get_data(gc).chat_conv_ids)
        if (it.second == chat_id)
            return it.first;

    return 0;
}


uint64 conv_id_to_chat_id(PurpleConnection* gc, int conv_id)
{
    for (const pair<int, uint64>& it: get_data(gc).chat_conv_ids)
        if (it.first == conv_id)
            return it.second;

    return 0;
}

int add_new_conv_id(PurpleConnection* gc, uint64 chat_id)
{
    int conv_id = 1;
    VkData& gc_data = get_data(gc);
    for (const pair<int, uint64>& it: gc_data.chat_conv_ids)
        if (it.first >= conv_id)
            conv_id = it.first + 1;

    // We probably do not open more than one conversation per second, so the keys will be exhausted in 2 ** 31 seconds,
    // quite a lot of time.
    gc_data.chat_conv_ids.emplace_back(conv_id, chat_id);
    return conv_id;
}


void remove_conv_id(PurpleConnection* gc, int conv_id)
{
    erase_if(get_data(gc).chat_conv_ids, [=](const pair<int, uint64>& p) {
        return p.first == conv_id;
    });
}

namespace
{

// Checks if all users are the same as listed in info, returns false if differ.
bool are_equal_chat_users(PurpleConnection* gc, PurpleConvChat* conv, VkChatInfo* info)
{
    set<string> names;
    for (const auto& p: info->participants)
        names.insert(p.second);
    const char* self_alias = purple_account_get_alias(purple_connection_get_account(gc));
    names.insert(self_alias);

    int users_size = 0;
    for (GList* it = purple_conv_chat_get_users(conv); it; it = it->next) {
        PurpleConvChatBuddy* cb = (PurpleConvChatBuddy*)it->data;
        if (!contains(names, purple_conv_chat_cb_get_name(cb)))
            return false;
        users_size++;
    }

    return (int)names.size() == users_size;
}

// Updates open conversation.
void update_open_chat_conv_impl(PurpleConnection* gc, PurpleConversation* conv, uint64 chat_id)
{
    VkChatInfo* info = get_chat_info(gc, chat_id);
    if (!info)
        return;

    if (purple_conversation_get_title(conv) != info->title.data())
        purple_conversation_set_title(conv, info->title.data());

    // Try to check if all users are present.
    if (!are_equal_chat_users(gc, PURPLE_CONV_CHAT(conv), info)) {
        vkcom_debug_info("Updating users in chat %" PRIu64 "\n", chat_id);

        purple_conv_chat_clear_users(PURPLE_CONV_CHAT(conv));

        for (const auto& p: info->participants) {
            uint64 user_id = p.first;
            const string& user_name = p.second;

            PurpleConvChatBuddyFlags flags;
            if (user_id == info->admin_id)
                flags = PURPLE_CBFLAGS_FOUNDER;
            else
                flags = PURPLE_CBFLAGS_NONE;
            purple_conv_chat_add_user(PURPLE_CONV_CHAT(conv), user_name.data(), "", flags, false);
        }
    }
}

}

void open_chat_conv(PurpleConnection* gc, uint64 chat_id, const SuccessCb& success_cb)
{
    if (chat_id_to_conv_id(gc, chat_id)) {
        if (success_cb)
            success_cb();
        return;
    }

    add_chat_if_needed(gc, chat_id, [=] {
        VkChatInfo* info = get_chat_info(gc, chat_id);
        if (!info)
            return;

        string name = chat_name_from_id(chat_id);
        int conv_id = add_new_conv_id(gc, chat_id);
        PurpleConversation* conv = serv_got_joined_chat(gc, conv_id, name.data());
        vkcom_debug_info("Added chat conversation %d for %s\n", conv_id, name.data());

        update_open_chat_conv_impl(gc, conv, chat_id);

        if (success_cb)
            success_cb();
    });
}


void update_open_chat_conv(PurpleConnection* gc, int conv_id)
{
    uint64 chat_id = conv_id_to_chat_id(gc, conv_id);
    if (chat_id == 0) {
        vkcom_debug_error("Trying to update unknown chat %d\n", conv_id);
        return;
    }

    PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
    if (!conv) {
        vkcom_debug_error("Unable to find chat%" PRIu64 "\n", chat_id);
        return;
    }

    update_open_chat_conv_impl(gc, conv, chat_id);
}


void update_chat_conv(PurpleConnection* gc, uint64 chat_id)
{
    PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
    if (!conv)
        return;

    update_open_chat_conv_impl(gc, conv, chat_id);
}

void update_all_open_chat_convs(PurpleConnection* gc)
{
    for (pair<int, uint64> p: get_data(gc).chat_conv_ids)
        update_open_chat_conv(gc, p.first);
}


uint64 find_user_id_from_conv(PurpleConnection* gc, int conv_id, const char* who)
{
    uint64 chat_id = conv_id_to_chat_id(gc, conv_id);
    if (chat_id == 0) {
        vkcom_debug_error("Asking for name %s in unknown chat %d\n", who, conv_id);
        return 0;
    }

    VkChatInfo* chat_info = get_chat_info(gc, chat_id);
    if (!chat_info) {
        vkcom_debug_error("Unknown chat%" PRIu64 "\n", chat_id);
        return 0;
    }

    for (const auto& p: chat_info->participants)
        if (p.second == who)
            return p.first;

    vkcom_debug_error("Unknown user %s in chat%" PRIu64 "\n", who, chat_id);
    return 0;
}

namespace
{

// Writes an error message about adding the user to the chat.
void show_add_user_error(PurpleConnection* gc, uint64 chat_id, uint64 user_id)
{
    PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
    string error_msg = str_format("Unable to add user %" PRIu64, user_id);
    purple_conversation_write(conv, nullptr, error_msg.data(), PURPLE_MESSAGE_ERROR, time(nullptr));
}

// Adds user to corresponding chat_info participants.
void add_user_to_chat_info(PurpleConnection* gc, uint64 chat_id, uint64 user_id)
{
    VkChatInfo& info = get_data(gc).chat_infos[chat_id];
    string user_name = get_user_display_name(gc, user_id);
    for (const pair<uint64, string>& p: info.participants) {
        // Somehow we have already added this user.
        if (user_id == p.first)
            return;

        if (user_name == p.second) {
            user_name = get_unique_display_name(gc, user_id);
            break;
        }
    }

    info.participants[user_id] = user_name;
}

// Writes an error message about setting title to the chat.
void show_set_title_error(PurpleConnection* gc, uint64 chat_id)
{
    PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
    string error_msg = str_format("Unable to set chat title");
    purple_conversation_write(conv, nullptr, error_msg.data(), PURPLE_MESSAGE_ERROR, time(nullptr));
}

}

void add_user_to_chat(PurpleConnection* gc, uint64 chat_id, uint64 user_id)
{
    CallParams params = {
        { "chat_id", to_string(chat_id) },
        { "user_id", to_string(user_id) }
    };
    vk_call_api(gc, "messages.addChatUser", params, [=] (const picojson::value& result) {
        if (!result.is<double>()) {
            show_add_user_error(gc, chat_id, user_id);
            return;
        }

        if (result.get<double>() != 1.0) {
            show_add_user_error(gc, chat_id, user_id);
            return;
        }

        add_user_to_chat_info(gc, chat_id, user_id);
        update_chat_conv(gc, chat_id);
    }, [=] (const picojson::value&) {
        show_add_user_error(gc, chat_id, user_id);
    });
}


void set_chat_title(PurpleConnection* gc, uint64 chat_id, const char* title)
{
    // We need to store title to update it in callback.
    string title_str = title;
    CallParams params = {
        { "chat_id", to_string(chat_id) },
        { "title", title_str }
    };
    vk_call_api(gc, "messages.editChat", params, [=](const picojson::value& result) {
        if (!result.is<double>()) {
            show_set_title_error(gc, chat_id);
            return;
        }

        if (result.get<double>() != 1.0) {
            show_set_title_error(gc, chat_id);
            return;
        }

        VkChatInfo& info = get_data(gc).chat_infos[chat_id];
        info.title = title_str;
        update_chat_conv(gc, chat_id);
    }, [=](const picojson::value&) {
        show_set_title_error(gc, chat_id);
    });
}
