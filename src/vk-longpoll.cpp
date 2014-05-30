#include "common.h"

#include <ctime>
#include <debug.h>
#include <server.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-chat.h"
#include "vk-common.h"
#include "vk-message-recv.h"
#include "vk-utils.h"

#include "vk-longpoll.h"

namespace
{

// NOTE: Re last_msg_id: last_msg_id is the id of the last message we have processed (either sent or received).
// It is permanently stored along with account information and is equal zero upon creation of account.
//
// Message ids are guaranteed to be monotonously increasing for each account (see message.get parameters).
//
// NOTE: Longpoll processing is the only place where we modify last_msg_id, because we receive
// events on both incoming and outgoing messages. Therefore, there are no races when updating
// last_msg_id (there will be in future when we switch to asynchronous loading of message history
// in the background).

// Loads last_msg_id from settings.
uint64 load_last_msg_id(PurpleConnection* gc);
// Saves last_msg_id to settings.
void save_last_msg_id(PurpleConnection* gc, uint64 last_msg_id);

// Helper for start_long_poll.
void start_long_poll_impl(PurpleConnection* gc, uint64 last_msg_id);

} // End of anonymous namespace

void start_long_poll(PurpleConnection* gc)
{
    uint64 last_msg_id = load_last_msg_id(gc);
    vkcom_debug_info("Starting Long Poll with last msg id %" PRIu64 "\n", last_msg_id);
    start_long_poll_impl(gc, last_msg_id);
}

namespace
{

uint64 load_last_msg_id(PurpleConnection* gc)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    return purple_account_get_int(account, "last_msg_id", 0);
}

void save_last_msg_id(PurpleConnection* gc, uint64 last_msg_id)
{
    PurpleAccount* account = purple_connection_get_account(gc);
    return purple_account_set_int(account, "last_msg_id", last_msg_id);
}

// Helper struct for request_long_poll.
struct LastMsg
{
    // The last message id we've processed.
    uint64 id;
    // There are no guarantees that messages ids in longpoll updates are increasing, so we cannot simply ignore
    // all the messages with id less than LastMsg.id. On the other hand, we must ignore all the messages,
    // processed via receive_messages_range in start_long_poll. ignored is the max message id received in
    // receive_messages_range, all messages with ids less or equal to it must be ignored.
    const uint64 ignored;
};

// Connects to given Long Poll server and starts reading events from it. last_msg_id is explained earlier,
// last_msg_id_from_start is a bit more complex. There are cases when request_long_poll will receive messages,
// which have already been processed:
void request_long_poll(PurpleConnection* gc, const string& server, const string& key, uint64 ts, LastMsg last_msg);
// Disconnects account on Long Poll errors as we do not have anything to do after that really.
void long_poll_fatal(PurpleConnection* gc);

void start_long_poll_impl(PurpleConnection* gc, uint64 last_msg_id)
{
    CallParams params = { {"use_ssl", "1"} };
    vk_call_api(gc, "messages.getLongPollServer", params, [=](const picojson::value& v) {
        if (!v.is<picojson::object>() || !field_is_present<string>(v, "key")
                || !field_is_present<string>(v, "server") || !field_is_present<double>(v, "ts")) {
            vkcom_debug_error("Strange response from messages.getLongPollServer: %s\n",
                               v.serialize().data());
            long_poll_fatal(gc);
            return;
        }

        // First, we update buddy presence and receive unread messages and only then start processing
        // events. We won't miss any events because we already got starting timestamp from server.
        update_friends_presence(gc, [=] {
            // Start updaing user and chat infos, buddy list.
            update_user_chat_infos(gc);
            receive_messages_range(gc, last_msg_id,  [=](uint64 max_msg_id) {
                // We've received no new messages.
                if (max_msg_id == 0)
                    max_msg_id = last_msg_id;
                else
                    save_last_msg_id(gc, max_msg_id);

                if (!field_is_present<string>(v, "server") || !field_is_present<string>(v, "key")
                        || !field_is_present<double>(v, "ts")) {
                    vkcom_debug_error("Wrong response from messages.getLongPollServer: %s\n",
                                       v.serialize().data());
                    long_poll_fatal(gc);
                    return;
                }

                const string& server = v.get("server").get<string>();
                const string& key = v.get("key").get<string>();
                double ts = v.get("ts").get<double>();
                request_long_poll(gc, server, key, ts, { max_msg_id, max_msg_id });
            });
        });
    }, [=](const picojson::value&) {
        long_poll_fatal(gc);
    });
}

// Reads and processes an event from updates array.
void process_update(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg);

// We request platform to detect desktop/mobile status and attachments to get "from"
// in chats.
const char* long_poll_url = "https://%s?act=a_check&key=%s&ts=%llu&wait=25&mode=66";

void request_long_poll(PurpleConnection* gc, const string& server, const string& key, uint64 ts,
                       LastMsg last_msg)
{
    string server_url = str_format(long_poll_url, server.data(), key.data(), ts);
#if 0
    vkcom_debug_info("Connecting to Long Poll %s\n", server_url.data());
#endif

    http_get(gc, server_url, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        // Connection has been cancelled due to account being disconnected.
        if (get_data(gc).is_closing())
            return;

        if (purple_http_response_get_code(response) != 200) {
            vkcom_debug_error("Error while reading response from Long Poll server: %s\n",
                               purple_http_response_get_error(response));
            long_poll_fatal(gc);
            return;
        }

        const char* response_text = purple_http_response_get_data(response, nullptr);
        const char* response_text_copy = response_text; // Picojson updates iterators it received.
        picojson::value root;
        string error = picojson::parse(root, response_text, response_text + strlen(response_text));
        if (!error.empty()) {
            vkcom_debug_error("Error parsing %s: %s\n", response_text_copy, error.data());
            long_poll_fatal(gc);
            return;
        }
        if (!root.is<picojson::object>()) {
            vkcom_debug_error("Strange response from Long Poll: %s\n", response_text_copy);
            long_poll_fatal(gc);
            return;
        }

        if (root.contains("failed")) {
            vkcom_debug_info("Long Poll got tired, re-requesting Long Poll server address\n");
            start_long_poll_impl(gc, last_msg.id);
            return;
        }

        if (!field_is_present<double>(root, "ts") || !field_is_present<picojson::array>(root, "updates")) {
            vkcom_debug_error("Strange response from Long Poll: %s\n", response_text_copy);
            long_poll_fatal(gc);
            return;
        }

        LastMsg next_last_msg = last_msg;

        const picojson::array& updates = root.get("updates").get<picojson::array>();
        for (const picojson::value& v: updates)
            process_update(gc, v, next_last_msg);

        uint64 next_ts = root.get("ts").get<double>();
        request_long_poll(gc, server, key, next_ts, next_last_msg);
    });
}

// Update codes coming from Long Poll
enum LongPollCodes
{
    LONG_POLL_MESSAGE_DELETED = 0,
    LONG_POLL_FLAGS_RESET = 1,
    LONG_POLL_FLAGS_SET = 2,
    LONG_POLL_FLAGS_CLEAR = 3,
    LONG_POLL_MESSAGE = 4,
    LONG_POLL_ONLINE = 8,
    LONG_POLL_OFFLINE = 9,
    LONG_POLL_CHAT_PARAMS_UPDATED = 51,
    LONG_POLL_USER_STARTED_TYPING = 61,
    LONG_POLL_USER_STARTED_CHAT_TYPING = 62,
    LONG_POLL_USER_CALLED = 70
};

// Flags, which can be present for message.
enum MessageFlags
{
    MESSAGE_FLAG_UNREAD = 1,
    MESSAGE_FLAGS_OUTBOX = 2,
    MESSAGE_FLAG_REPLIED = 4,
    MESSAGE_FLAG_IMPORTANT = 8,
    MESSAGE_FLAG_CHAT = 16,
    MESSAGE_FLAG_FRIENDS = 32,
    MESSAGE_FLAG_SPAM = 64,
    MESSAGE_FLAG_DELETED = 128,
    MESSAGE_FLAG_FIXED = 256,
    MESSAGE_FLAG_MEDIA = 512
};

// Processes message event.
void process_message(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg);
// Processes user online/offline event.
void process_online(PurpleConnection* gc, const picojson::value& v, bool online);
// Processes update of chat parameters.
void process_chat_update(PurpleConnection* gc, const picojson::value& v);
// Processes user typing event.
void process_typing(PurpleConnection* gc, const picojson::value& v);

void process_update(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg)
{
    if (!v.is<picojson::array>() || !v.contains(0)) {
        vkcom_debug_error("Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }

    int code = v.get(0).get<double>();
    switch (code) {
    case LONG_POLL_MESSAGE:
        process_message(gc, v, last_msg);
        break;
    case LONG_POLL_ONLINE:
        process_online(gc, v, true);
        break;
    case LONG_POLL_OFFLINE:
        process_online(gc, v, false);
        break;
    case LONG_POLL_CHAT_PARAMS_UPDATED:
        process_chat_update(gc, v);
        break;
    case LONG_POLL_USER_STARTED_TYPING:
        process_typing(gc, v);
        break;
    default:
        break;
    }
}

// Process incoming and outgoing messages respectively. In general, there is duplication between these functions
// and vk-message-recv code, they should somehow be refactored.
void process_incoming_message_internal(PurpleConnection* gc, uint64 msg_id, int flags, uint64 user_id, string text,
                                       uint64 timestamp, const picojson::value *attachments);
void process_outgoing_message_internal(PurpleConnection* gc, uint64 msg_id, int flags, uint64 user_id, string text,
                                       uint64 timestamp);

void process_message(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg)
{
    if (!v.contains(6) || !v.get(1).is<double>() || !v.get(2).is<double>() || !v.get(3).is<double>()
            || !v.get(4).is<double>() || !v.get(6).is<string>()) {
        vkcom_debug_error("Strange response from Long Poll in updates: %s\n", v.serialize().data());
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Unable to receive message");
        return;
    }
    uint64 msg_id = v.get(1).get<double>();
    // Check if we already processed this message in receive_messages_range.
    if (msg_id <= last_msg.ignored)
        return;

    if (msg_id > last_msg.id) {
        last_msg.id = msg_id;
        // Pidgin defaults saving to once per 5 seconds, so there is no problem with resetting this value frequently.
        save_last_msg_id(gc, msg_id);
    }

    int flags = v.get(2).get<double>();

    uint64 user_id = v.get(3).get<double>();
    uint64 timestamp = v.get(4).get<double>();
    // NOTE:
    // * The text is simple UTF-8 text with some HTML leftovers:
    //   * The only tag which it may contain is <br> (API v5.0 stopped using <br>, but Long Poll still
    //     sends <br>).
    //   * &amp; &lt; &gt; &quot; are escaped.
    // * Links are sent as plaintext, both Vk.com and Pidgin linkify messages automatically.
    // * Smileys are returned as Unicode emoji (both emoji and pseudocode smileys are accepted on message send).
    string text = v.get(6).get<string>();

    const picojson::value* attachments = nullptr;
    if (v.contains(7))
        attachments = &v.get(7);

    if (!(flags & MESSAGE_FLAGS_OUTBOX)) {
        // Processing incoming message
        vkcom_debug_info("Got incoming message from %" PRIu64 "\n", user_id);

        process_incoming_message_internal(gc, msg_id, flags, user_id, std::move(text), timestamp, attachments);
    } else {
        // Process outgoing message. This message could've been sent either by us, or by another connected client.
        // See description in vk-common.h of corresponding members of VkData for details.
        vkcom_debug_info("Got outgoing message\n");

        VkData& gc_data = get_data(gc);
        // The message has been sent by us, ignore it.
        if (gc_data.remove_sent_msg_id(msg_id))
            return;

        steady_duration since_last_msg_sent = steady_clock::now() - gc_data.last_msg_sent_time();
        if (to_milliseconds(since_last_msg_sent) >= 5000) {
            // This is fast path: the message is guaranteed to be sent from someplace else, no need for timeout.
            process_outgoing_message_internal(gc, msg_id, flags, user_id, std::move(text), timestamp);
            return;
        }

        // The last message, which has been sent by us, has been sent not long ago (i.e. less than 1 second).
        purple_debug_warning("prpl-vkcom", "We sent message not long ago, let's have a check after timeout\n");
        timeout_add(gc, 5000, [=] {
            // Check again after 5 seconds, whether we sent the message or not.
            if (get_data(gc).remove_sent_msg_id(msg_id))
                return false;

            purple_debug_warning("prpl-vkcom", "We have sent a message not long ago, but not all msg id"
                                 "are belong to us (msg id %" PRIu64 ")\n", msg_id);
            process_outgoing_message_internal(gc, msg_id, flags, user_id, std::move(text), timestamp);
            return false;
        });
    }
}

// NOTE: Chat messages are sent with chat_id + CHAT_ID_OFFSET as user id, unfortunately, no user id
// is stored, so we have to call messages.get.
const uint64 CHAT_ID_OFFSET = 2000000000LL;

const uint PLATFORM_WEB = 7;

void process_incoming_message_internal(PurpleConnection* gc, uint64 msg_id, int flags, uint64 user_id, string text,
                                       uint64 timestamp, const picojson::value* attachments)
{
    // NOTE:
    //  There are two ways of processing messages with attachments:
    //   a) either we can get attachement ids (photo ids, audio ids etc.) from Long Poll event and get information
    //      via photo.getById et al.
    //   or b) we can get all the messages with attachments via receive_messages.
    //  While the first way seems preferable at first, it has quite a number of problems:
    //   * access to information on private images/documents/etc. (e.g. ones uploaded from Vk.com chat UI)
    //     is prohibited and we can only show links to the corresponding page;
    //   * there is no video.getById so we can show no information on video;
    //   * it takes at least one additional call per message (receive_messages takes exactly one call).
    if (flags & MESSAGE_FLAG_MEDIA) {
        receive_messages(gc, { msg_id });
    } else {
        replace_emoji_with_text(text);

        if (user_id < CHAT_ID_OFFSET) {
            add_buddy_if_needed(gc, user_id, [=] {
                serv_got_im(gc, user_name_from_id(user_id).data(), text.data(), PURPLE_MESSAGE_RECV, timestamp);
                mark_message_as_read(gc, { VkReceivedMessage{ msg_id, user_id, 0 } });
            });
        } else {
            uint64 chat_id = user_id - CHAT_ID_OFFSET;

            if (!attachments || !attachments->contains("from") || !attachments->get("from").is<string>()) {
                vkcom_debug_error("Chat message has wrong attachments: %s\n", attachments
                                  ? attachments->serialize().data() : "null");
                // Let's try to receive the message the other way.
                receive_messages(gc, { msg_id });
                return;
            }

            const string& from_user_id_str = attachments->get("from").get<string>();
            uint64 from_user_id = atoll(from_user_id_str.data());
            if (from_user_id == 0) {
                vkcom_debug_error("Chat message has wrong attachments: %s\n", attachments
                                  ? attachments->serialize().data() : "null");
                // Let's try to receive the message the other way.
                receive_messages(gc, { msg_id });
                return;
            }

            // TODO: Remove code duplication with vk-message-recv.cpp
            open_chat_conv(gc, chat_id, [=] {
                int conv_id = chat_id_to_conv_id(gc, chat_id);
                string from = get_user_display_name(gc, from_user_id, chat_id);
                serv_got_chat_in(gc, conv_id, from.data(), PURPLE_MESSAGE_RECV, text.data(), timestamp);
                mark_message_as_read(gc, { VkReceivedMessage{ msg_id, from_user_id, chat_id } });
            });
        }
    }
}

void process_outgoing_message_internal(PurpleConnection* gc, uint64 msg_id, int flags, uint64 user_id, string text,
                                       uint64 timestamp)
{
    // See NOTE in process_incoming_message_internal. Unlik incoming messages, we know perfectly well who is
    // the message author for outgoing messages.
    if (flags & MESSAGE_FLAG_MEDIA) {
        receive_messages(gc, { msg_id });
    } else {
        replace_emoji_with_text(text);

        // Check if the conversation is open, so that we write to the conversation, not the log.
        // TODO: Remove code duplication with vk-message-recv.cpp
        if (user_id < CHAT_ID_OFFSET) {
            PurpleConversation* conv = find_conv_for_id(gc, user_id, 0);
            string from = purple_account_get_name_for_display(purple_connection_get_account(gc));
            if (conv) {
                purple_conv_im_write(PURPLE_CONV_IM(conv), from.data(), text.data(), PURPLE_MESSAGE_SEND, timestamp);
            } else {
                PurpleLogCache logs(gc);
                PurpleLog* log = logs.for_user(user_id);
                purple_log_write(log, PURPLE_MESSAGE_SEND, from.data(), timestamp, text.data());
            }
        } else {
            uint64 chat_id = user_id - CHAT_ID_OFFSET;
            PurpleConversation* conv = find_conv_for_id(gc, 0, chat_id);
            string from = get_self_chat_display_name(gc);
            if (conv) {
                purple_conv_chat_write(PURPLE_CONV_CHAT(conv), from.data(), text.data(), PURPLE_MESSAGE_SEND, timestamp);
            } else {
                PurpleLogCache logs(gc);
                PurpleLog* log = logs.for_chat(chat_id);
                purple_log_write(log, PURPLE_MESSAGE_SEND, from.data(), timestamp, text.data());
            }
        }
    }
}

void process_online(PurpleConnection* gc, const picojson::value& v, bool online)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        vkcom_debug_error("Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    if (v.get(1).get<double>() > 0) {
        vkcom_debug_error("Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    uint64 user_id = -v.get(1).get<double>();
    string name = user_name_from_id(user_id);

    vkcom_debug_info("User %s changed online to %d\n", name.data(), online);

    if (!user_in_buddy_list(gc, user_id)) {
        vkcom_debug_info("User %s has come online, but is not present in buddy list."
                          "He has probably been added behind our backs.", name.data());
        add_buddy_if_needed(gc, user_id);
        return;
    } else {
        VkUserInfo* info = get_user_info(gc, user_id);
        if (!info) {
            vkcom_debug_error("We somehow do not have info on user %s\n", name.data());
            return;
        }

        if (online) {
            if (!v.contains(2) || !v.get(2).is<double>()) {
                vkcom_debug_error("Strange response from Long Poll in updates: %s\n",
                                   v.serialize().data());
                return;
            }
            uint platform = uint64(v.get(2).get<double>()) % 0x100;

            if (platform == PLATFORM_WEB) {
                info->online = true;
                info->online_mobile = false;
            } else {
                info->online = true;
                info->online_mobile = true;
            }

            PurpleAccount* account = purple_connection_get_account(gc);
            purple_prpl_got_user_login_time(account, name.data(), time(nullptr));
        } else {
            info->online = false;
            info->online_mobile = false;
        }
        update_presence_in_blist(gc, user_id);
    }
}

void process_chat_update(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        vkcom_debug_error("Strange respone form Long Poll in updates: %s\n",
                          v.serialize().data());
        return;
    }
    uint64 chat_id = v.get(1).get<double>();

    vkcom_debug_info("Updating parameters for chat %" PRIu64 "\n", chat_id);

    update_chat_infos(gc, { chat_id }, nullptr, true);
}

void process_typing(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        vkcom_debug_error("Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    uint64 user_id = v.get(1).get<double>();

    add_buddy_if_needed(gc, user_id, [=] {
        // Vk.com documentation states, that "user is typing" messages are sent with ~10 second
        // interval between them. Let's make it 11, just to be sure.
        serv_got_typing(gc, user_name_from_id(user_id).data(), 11, PURPLE_TYPING);
    });
}

void long_poll_fatal(PurpleConnection* gc)
{
    vkcom_debug_error("Unable to connect to long-poll server, connection will be terminated\n");
    purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Unable to connect to Long Poll server");
}

} // End of anonymous namespace
