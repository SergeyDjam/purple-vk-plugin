#include "common.h"

#include <ctime>
#include <debug.h>
#include <server.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-message-recv.h"

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
void start_long_poll_internal(PurpleConnection* gc, uint64 last_msg_id);

} // End of anonymous namespace

void start_long_poll(PurpleConnection* gc)
{
    uint64 last_msg_id = load_last_msg_id(gc);
    start_long_poll_internal(gc, last_msg_id);
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
    // There are no guarantees, that messages ids in longpoll updates are increasing, so we cannot simply ignore
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

void start_long_poll_internal(PurpleConnection* gc, uint64 last_msg_id)
{
    CallParams params = { {"use_ssl", "1"} };
    vk_call_api(gc, "messages.getLongPollServer", params, [=](const picojson::value& v) {
        if (!v.is<picojson::object>() || !field_is_present<string>(v, "key")
                || !field_is_present<string>(v, "server") || !field_is_present<double>(v, "ts")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.getLongPollServer: %s\n",
                               v.serialize().data());
            long_poll_fatal(gc);
            return;
        }

        // First, we update buddy presence and receive unread messages and only then start processing
        // events. We won't miss any events because we already got starting timestamp from server.
        update_buddy_list(gc, true, [=] {
            receive_messages_range(gc, last_msg_id,  [=](uint64 max_msg_id) {
                // We've received no new messages.
                if (max_msg_id == 0)
                    max_msg_id = last_msg_id;

                if (!field_is_present<string>(v, "server") || !field_is_present<string>(v, "key")
                        || !field_is_present<double>(v, "ts")) {
                    purple_debug_error("prpl-vkcom", "Wrong response from messages.getLongPollServer: %s\n",
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

const char* long_poll_url = "https://%s?act=a_check&key=%s&ts=%llu&wait=25";

void request_long_poll(PurpleConnection* gc, const string& server, const string& key, uint64 ts,
                       LastMsg last_msg)
{
    VkConnData* conn_data = get_conn_data(gc);

    string server_url = str_format(long_poll_url, server.data(), key.data(), ts);
    purple_debug_info("prpl-vkcom", "Connecting to Long Poll %s\n", server_url.data());

    http_get(gc, server_url, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        // Connection has been cancelled due to account being disconnected.
        if (conn_data->is_closing())
            return;

        if (purple_http_response_get_code(response) != 200) {
            purple_debug_error("prpl-vkcom", "Error while reading response from Long Poll server: %s\n",
                               purple_http_response_get_error(response));
            request_long_poll(gc, server, key, ts, last_msg);
            return;
        }

        const char* response_text = purple_http_response_get_data(response, nullptr);
        const char* response_text_copy = response_text; // Picojson updates iterators it received.
        picojson::value root;
        string error = picojson::parse(root, response_text, response_text + strlen(response_text));
        if (!error.empty()) {
            purple_debug_error("prpl-vkcom", "Error parsing %s: %s\n", response_text_copy, error.data());
            request_long_poll(gc, server, key, ts, last_msg);
            return;
        }
        if (!root.is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Strange response from Long Poll: %s\n", response_text_copy);
            request_long_poll(gc, server, key, ts, last_msg);
            return;
        }

        if (root.contains("failed")) {
            purple_debug_info("prpl-vkcom", "Long Poll got tired, re-requesting Long Poll server address\n");
            start_long_poll_internal(gc, last_msg.id);
            return;
        }

        if (!field_is_present<double>(root, "ts") || !field_is_present<picojson::array>(root, "updates")) {
            purple_debug_error("prpl-vkcom", "Strange response from Long Poll: %s\n", response_text_copy);
            request_long_poll(gc, server, key, ts, last_msg);
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
// Processes user typing event.
void process_typing(PurpleConnection* gc, const picojson::value& v);

void process_update(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg)
{
    if (!v.is<picojson::array>() || !v.contains(0)) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
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
    case LONG_POLL_USER_STARTED_TYPING:
        process_typing(gc, v);
        break;
    default:
        break;
    }
}

void process_message(PurpleConnection* gc, const picojson::value& v, LastMsg& last_msg)
{
    if (!v.contains(6) || !v.get(1).is<double>() || !v.get(2).is<double>() || !v.get(3).is<double>()
            || !v.get(4).is<double>() || !v.get(6).is<string>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    uint64 mid = v.get(1).get<double>();
    // Check if we already processed this message in receive_messages_range.
    if (mid <= last_msg.ignored)
        return;

    if (mid > last_msg.id) {
        last_msg.id = mid;
        // Pidgin defaults saving to once per 5 seconds, so there is no problem with resetting this value frequently.
        save_last_msg_id(gc, mid);
    }

    int flags = v.get(2).get<double>();

    uint64 uid = v.get(3).get<double>();
    uint64 timestamp = v.get(4).get<double>();
    // NOTE:
    // * The text is simple UTF-8 text with some HTML leftovers:
    //   * The only tag which it may contain is <br> (API v5.0 stopped using <br>, but Long Poll still
    //     sends <br>).
    //   * &amp; &lt; &gt; &quot; are escaped.
    // * Links are sent as plaintext, both Vk.com and Pidgin linkify messages automatically.
    // * Smileys are returned as Unicode emoji (both emoji and pseudocode smileys are accepted on message send).
    string text = v.get(6).get<string>();

    // TODO: Process outgoing message. This message could've been sent either by us, or by another connected client.
    if (flags & MESSAGE_FLAGS_OUTBOX)
        return;

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
        receive_messages(gc, { mid });
    } else {
        // There are no attachments. Yes, messages with attached documents are also marked as media.
        serv_got_im(gc, buddy_name_from_uid(uid).data(), text.data(), PURPLE_MESSAGE_RECV, timestamp);
        mark_message_as_read(gc, { mid });
    }
}

void process_online(PurpleConnection* gc, const picojson::value& v, bool online)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    if (v.get(1).get<double>() > 0) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    uint64 uid = -v.get(1).get<double>();
    string name = buddy_name_from_uid(uid);

    purple_debug_info("prpl-vkcom", "User %s changed online to %d\n", name.data(), online);

    PurpleAccount* account = purple_connection_get_account(gc);
    if (!purple_find_buddy(account, name.data())) {
        purple_debug_info("prpl-vkcom", "User %s has come online, but is not present in buddy list."
                          "He has probably been added behind our backs.", name.data());
        update_buddies(gc, { uid });
        return;
    }

    if (online) {
        purple_prpl_got_user_status(account, name.data(), "online", nullptr);
        purple_prpl_got_user_login_time(account, name.data(), time(nullptr));
    } else {
        purple_prpl_got_user_status(account, name.data(), "offline", nullptr);
    }
}

void process_typing(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().data());
        return;
    }
    uint64 uid = v.get(1).get<double>();

    serv_got_typing(gc, buddy_name_from_uid(uid).data(), 6000, PURPLE_TYPING);
}

void long_poll_fatal(PurpleConnection* gc)
{
    purple_debug_error("prpl-vkcom", "Unable to connect to long-poll server, connection will be terminated\n");
    // We could've added idle_add similar to timeout_add, but why bother?
    timeout_add(gc, 1, [=] {
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Unable to connect to Long Poll server");
        return false;
    });
}

} // End of anonymous namespace
