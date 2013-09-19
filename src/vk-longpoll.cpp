#include "common.h"

#include <ctime>
#include <debug.h>
#include <server.h>

#include "httputils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-message.h"
#include "utils.h"

#include "vk-longpoll.h"

namespace
{

// Connects to given Long Poll server and starts reading events from it.
void request_long_poll(PurpleConnection* gc, const string& server, const string& key, uint64 ts);
// Disconnects account on Long Poll errors as we do not have anything to do after that really.
void long_poll_fatal(PurpleConnection* gc);

} // End of anonymous namespace

void start_long_poll(PurpleConnection* gc)
{
    CallParams params = { {"use_ssl", "1"} };
    vk_call_api(gc, "messages.getLongPollServer", params, [=](const picojson::value& v) {
        if (!v.is<picojson::object>() || !field_is_present<string>(v, "key")
                || !field_is_present<string>(v, "server") || !field_is_present<double>(v, "ts")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.getLongPollServer: %s\n",
                               v.serialize().c_str());
            long_poll_fatal(gc);
            return;
        }

        // First, we update buddy presence and receive unread messages and only then start processing
        // events. We won't miss any events because we already got starting timestamp from server.
        update_buddy_list(gc, [=] {
            receive_unread_messages(gc, [=] {
                request_long_poll(gc, v.get("server").get<string>(), v.get("key").get<string>(),
                                  v.get("ts").get<double>());
            });
        }, true);
    }, [=](const picojson::value&) {
        long_poll_fatal(gc);
    });
}

namespace
{

// Reads and processes an event from updates array.
void process_update(PurpleConnection* gc, const picojson::value& v);

const char* long_poll_url = "https://%s?act=a_check&key=%s&ts=%llu&wait=25";

void request_long_poll(PurpleConnection* gc, const string& server, const string& key, uint64 ts)
{
    VkConnData* conn_data = (VkConnData*)purple_connection_get_protocol_data(gc);

    string server_url = str_format(long_poll_url, server.c_str(), key.c_str(), ts);
    purple_debug_info("prpl-vkcom", "Connecting to Long Poll %s\n", server_url.c_str());

    http_get(gc, server_url.c_str(), [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        // Connection has been cancelled due to account being disconnected.
        if (conn_data->is_closing())
            return;

        if (purple_http_response_get_code(response) != 200) {
            purple_debug_error("prpl-vkcom", "Error while reading response from Long Poll server: %s\n",
                               purple_http_response_get_error(response));
            request_long_poll(gc, server, key, ts);
            return;
        }

        const char* response_text = purple_http_response_get_data(response, nullptr);
        picojson::value root;
        string error = picojson::parse(root, response_text, response_text + strlen(response_text));
        if (!error.empty()) {
            purple_debug_error("prpl-vkcom", "Error parsing %s: %s\n", response_text, error.c_str());
            request_long_poll(gc, server, key, ts);
            return;
        }
        if (!root.is<picojson::object>()) {
            purple_debug_error("prpl-vkcom", "Strange response from Long Poll: %s\n", response_text);
            request_long_poll(gc, server, key, ts);
            return;
        }

        if (root.contains("failed")) {
            purple_debug_info("prpl-vkcom", "Long Poll got tired, re-requesting Long Poll server address\n");
            start_long_poll(gc);
            return;
        }

        if (!field_is_present<double>(root, "ts") || !field_is_present<picojson::array>(root, "updates")) {
            purple_debug_error("prpl-vkcom", "Strange response from Long Poll: %s\n", response_text);
            request_long_poll(gc, server, key, ts);
            return;
        }

        const picojson::array& updates = root.get("updates").get<picojson::array>();
        for (const picojson::value& v: updates)
            process_update(gc, v);

        uint64 next_ts = root.get("ts").get<double>();
        request_long_poll(gc, server, key, next_ts);
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
void process_message(PurpleConnection* gc, const picojson::value& v);
// Processes user online/offline event.
void process_online(PurpleConnection* gc, const picojson::value& v, bool online);
// Processes user typing event.
void process_typing(PurpleConnection* gc, const picojson::value& v);

void process_update(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.is<picojson::array>() || !v.contains(0)) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().c_str());
        return;
    }

    int code = v.get(0).get<double>();
    switch (code) {
    case LONG_POLL_MESSAGE:
        process_message(gc, v);
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

void process_message(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.contains(6) || !v.get(1).is<double>() || !v.get(2).is<double>() || !v.get(3).is<double>()
            || !v.get(4).is<double>() || !v.get(6).is<string>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().c_str());
        return;
    }
    int flags = v.get(2).get<double>();
    // Ignore outgoing messages.
    if (flags & MESSAGE_FLAGS_OUTBOX)
        return;

    uint64 mid = v.get(1).get<double>();
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
        serv_got_im(gc, buddy_name_from_uid(uid).c_str(), text.c_str(), PURPLE_MESSAGE_RECV, timestamp);
        mark_message_as_read(gc, { mid });
    }
}

void process_online(PurpleConnection* gc, const picojson::value& v, bool online)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().c_str());
        return;
    }
    if (v.get(1).get<double>() > 0) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().c_str());
        return;
    }
    uint64 uid = -v.get(1).get<double>();
    string name = buddy_name_from_uid(uid);

    purple_debug_info("prpl-vkcom", "User %s changed online to %d\n", name.c_str(), online);

    PurpleAccount* account = purple_connection_get_account(gc);
    if (!purple_find_buddy(account, name.c_str())) {
        purple_debug_info("prpl-vkcom", "User %s has come online, but is not present in buddy list."
                          "He has probably been added behind our backs.", name.c_str());
        update_buddy(gc, uid, nullptr, true);
        return;
    }

    if (online) {
        purple_prpl_got_user_status(account, name.c_str(), "online", nullptr);
        purple_prpl_got_user_login_time(account, name.c_str(), time(nullptr));
    } else {
        purple_prpl_got_user_status(account, name.c_str(), "offline", nullptr);
    }
}

void process_typing(PurpleConnection* gc, const picojson::value& v)
{
    if (!v.contains(1) || !v.get(1).is<double>()) {
        purple_debug_error("prpl-vkcom", "Strange response from Long Poll in updates: %s\n",
                           v.serialize().c_str());
        return;
    }
    uint64 uid = v.get(1).get<double>();

    serv_got_typing(gc, buddy_name_from_uid(uid).c_str(), 6000, PURPLE_TYPING);
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
