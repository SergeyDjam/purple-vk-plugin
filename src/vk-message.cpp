#include <debug.h>
#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "httputils.h"
#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-message.h"
#include "utils.h"

#include "vk-api.h"

namespace
{

// Helper struct used to reduce length of function signatures.
struct SendMessage
{
    uint64 uid;
    string message;
    SendSuccessCb success_cb;
    ErrorCb error_cb;
};

// Helper function, used in send_im_message and request_captcha.
void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid = "",
                              const string& captcha_key = "");

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, uint64 uid, const char* message,
                    const SendSuccessCb& success_cb, const ErrorCb& error_cb)
{
    // NOTE: We de-HTMLify message before sending, because
    //  * Vk.com chat is plaintext anyway
    //  * Vk.com accepts '\n' in place of <br>
    char* unescaped_message = purple_unescape_html(message);
    send_im_message_internal(gc, { uid, unescaped_message, success_cb, error_cb });
    g_free(unescaped_message);
    return 1;
}

namespace
{

// Process error and call either success_cb or error_cb. The only error which is meaningfully
// processed is CAPTCHA request.
void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message);

void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid,
                              const string& captcha_key)
{
    CallParams params = { {"user_id", to_string(message.uid)}, {"message", message.message},
                          {"type", "1"} };
    if (!captcha_sid.empty())
        params.emplace_back("captcha_sid", captcha_sid);
    if (!captcha_key.empty())
        params.emplace_back("captcha_key", captcha_key);
    vk_call_api(gc, "messages.send", params, [=](const picojson::value&) {
        if (message.success_cb)
            message.success_cb();
    }, [=](const picojson::value& error) {
        process_im_error(error, gc, message);
    });
}

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, uint64 uid, const SendMessage& message);

PurpleConversation* find_conv_for_uid(PurpleConnection* gc, uint64 uid)
{
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, buddy_name_from_uid(uid).c_str(),
                                                 purple_connection_get_account(gc));
}

void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message)
{
    if (!error.is<picojson::object>() || !field_is_present<double>(error, "error_code")) {
        // Most probably, network timeout.
        show_error(gc, message.uid, message);
        return;
    }
    int error_code = error.get("error_code").get<double>();
    if (error_code != VK_CAPTCHA_NEEDED) {
        show_error(gc, message.uid, message);
        return;
    }
    if (!field_is_present<string>(error, "captcha_sid") || !field_is_present<string>(error, "captcha_img")) {
        purple_debug_error("prpl-vkcom", "Captcha request does not contain captcha_sid or captcha_img");
        show_error(gc, message.uid, message);
        return;
    }

    const string& captcha_sid = error.get("captcha_sid").get<string>();
    const string& captcha_img = error.get("captcha_img").get<string>();
    purple_debug_info("prpl-vkcom", "Received CAPTCHA %s\n", captcha_img.c_str());

    request_captcha(gc, captcha_img, [=](const string& captcha_key) {
        send_im_message_internal(gc, message, captcha_sid, captcha_key);
    }, [=] {
        show_error(gc, message.uid, message);
    });
}

void show_error(PurpleConnection* gc, uint64 uid, const SendMessage& message)
{
    purple_debug_error("prpl-vkcom", "Error sending message to %llu: %s\n", (unsigned long long)message.uid,
                       message.message.c_str());

    PurpleConversation* conv = find_conv_for_uid(gc, uid);
    if (conv) {
        char* escaped_message = g_markup_escape_text(message.message.c_str(), -1);
        string error_msg = str_format("Error sending message '%s'", escaped_message);
        purple_conversation_write(conv, nullptr, error_msg.c_str(),
                                  PurpleMessageFlags(PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NO_LINKIFY), time(nullptr));
        g_free(escaped_message);
    }

    if (message.error_cb)
        message.error_cb();
}

} // End of anonymous namespace


unsigned send_typing_notification(PurpleConnection* gc, uint64 uid)
{
    CallParams params = { {"user_id", to_string(uid)}, {"type", "typing"} };
    vk_call_api(gc, "messages.setActivity", params);

    // Resend typing notification in 5 seconds
    return 5;
}


namespace
{

// Creates string of integers, separated by sep.
template<typename Sep, typename It>
string str_concat_int(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        char buf[128];
        sprintf(buf, "%lld", (long long)*it);
        s += buf;
    }
    return s;
}

template<typename Sep, typename C>
string str_concat_int(Sep sep, const C& c)
{
    return str_concat_int(sep, c.cbegin(), c.cend());
}

} // End of anonymous namespace

void mark_message_as_read(PurpleConnection* gc, const uint64_vec& message_ids)
{
    if (message_ids.empty())
        return;
    // Creates string of identifiers, separated with comma.
    string ids_str = str_concat_int(',', message_ids);

    CallParams params = { {"message_ids", ids_str} };
    vk_call_api(gc, "messages.markAsRead", params);
}

namespace
{

// Three reasons for creating a separate class:
//  a) messages.get returns answers in reverse time order, so we have to store messages and sort them later;
//  b) messages.get paginates the answers, so that multiple calls may be required in order to retrieve all
//     messages;
//  c) we have to run a bunch of HTTP requests to retrieve photo and video thumbnails and append them to
//     received messages.
// NOTE: This design (along with VkAuthenticator in vk-auth.cpp) seems to bee too heavyweight and Java-ish,
// but I cannot create any better. Any ideas?

class MessageReceiver
{
public:
    static MessageReceiver* create(PurpleConnection* gc, const ReceivedCb& recevied_cb);

    // Receives all unread messages.
    void run_unread();
    // Receives messages with given ids.
    void run(const uint64_vec& message_ids);

private:
    struct ReceivedMessage
    {
        uint64 uid;
        uint64 mid;
        string text;
        uint64 timestamp;

        // A list of thumbnail URLs to download and append to message. Set in process_attachments,
        // used in download_thumbnails.
        vector<string> thumbnail_urls;
    };
    vector<ReceivedMessage> m_messages;

    PurpleConnection* m_gc;
    ReceivedCb m_received_cb;

    MessageReceiver(PurpleConnection* gc, const ReceivedCb& received_cb)
        : m_gc(gc),
          m_received_cb(received_cb)
    {
    }

    // Runs messages.get from given offset.
    void run_unread(int offset);
    // Processes result of messages.get and messages.getById
    int process_result(const picojson::value& result);
    // Processes attachments: appends urls to message text, adds thumbnail_urls.
    void process_attachments(const picojson::array& items, ReceivedMessage& message) const;
    // Downloads the given thumbnail for given message, modifies corresponding message text
    // and calls either next download_thumbnail() or finish(). message is index into m_messages,
    // thumbnail is index into thumbnail_urls.
    void download_thumbnail(int message, int thumbnail);
    // Sorts received messages, sends them to libpurple client and destroys this.
    void finish();
};

} // End of anonymous namespace

void receive_unread_messages(PurpleConnection* gc, const ReceivedCb& received_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, received_cb);
    receiver->run_unread();
}

void receive_messages(PurpleConnection* gc, const uint64_vec& message_ids, const ReceivedCb& received_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, received_cb);
    receiver->run(message_ids);
}

namespace
{

MessageReceiver* MessageReceiver::create(PurpleConnection* gc, const ReceivedCb& recevied_cb)
{
    return new MessageReceiver(gc, recevied_cb);
}

void MessageReceiver::run_unread()
{
    run_unread(0);
}

void MessageReceiver::run(const uint64_vec& message_ids)
{
    string ids_str = str_concat_int(',', message_ids);
    CallParams params = { {"message_ids", ids_str} };
    vk_call_api(m_gc, "messages.getById", params, [=](const picojson::value& result) {
        process_result(result);
        download_thumbnail(0, 0);
    }, [=](const picojson::value&) {
        finish();
    });
}

void MessageReceiver::run_unread(int offset)
{
    CallParams params = { {"out", "0"}, {"filters", "1"}, {"offset", str_format("%d", offset)} };
    vk_call_api(m_gc, "messages.get", params, [=](const picojson::value& result) {
        int item_count = process_result(result);
        if (item_count == 0) {
            // We ignore "count" parameter in result and increase offset until it returns empty list.
            download_thumbnail(0, 0);
            return;
        }
        run_unread(offset + item_count);
    }, [=](const picojson::value&) {
        finish();
    });
}

int MessageReceiver::process_result(const picojson::value& result)
{
    if (!field_is_present<double>(result, "count") || !field_is_present<picojson::array>(result, "items")) {
        purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                           result.serialize().c_str());
        return 0;
    }

    const picojson::array& items = result.get("items").get<picojson::array>();
    for (const picojson::value& v: items) {
        if (!field_is_present<double>(v, "user_id") || !field_is_present<double>(v, "date")
                || !field_is_present<string>(v, "body") || !field_is_present<double>(v, "id")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               result.serialize().c_str());
            continue;
        }

        uint64 uid = v.get("user_id").get<double>();
        uint64 mid = v.get("id").get<double>();
        uint64 timestamp = v.get("date").get<double>();

        // NOTE:
        //  * We must escape text, otherwise we cannot receive comment, containing &amp; or <br> as libpurple
        //    will wrongfully interpret them as markup.
        //  * Links are returned as plaintext, linkified by Pidgin etc.
        //  * Smileys are returned as Unicode emoji (both emoji and pseudocode smileys are accepted on message send).
        char* escaped = purple_markup_escape_text(v.get("body").get<string>().c_str(), -1);
        string text = escaped;
        g_free(escaped);

        m_messages.push_back({ uid, mid, text, timestamp, {} });

        // Process attachments: append information to text.
        if (field_is_present<picojson::array>(v, "attachments"))
            process_attachments(v.get("attachments").get<picojson::array>(), m_messages.back());
    }
    return items.size();
}

void MessageReceiver::process_attachments(const picojson::array& items, ReceivedMessage& message) const
{
    for (const picojson::value& v: items) {
        if (!field_is_present<string>(v, "type")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().c_str());
            return;
        }
        const string& type = v.get("type").get<string>();
        if (!field_is_present<picojson::object>(v, type)) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().c_str());
            return;
        }
        const picojson::value& fields = v.get(type);

        message.text += "<br>";

        if (type == "photo") {
            if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
                    || !field_is_present<string>(fields, "text") || !field_is_present<string>(fields, "photo_604")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().c_str());
                continue;
            }
            const uint64 id = fields.get("id").get<double>();
            const int64 owner_id = fields.get("owner_id").get<double>();
            const string& photo_text = fields.get("text").get<string>();
            const string& thumbnail = fields.get("photo_604").get<string>();

            if (!photo_text.empty())
                message.text += str_format("<a href='http://vk.com/photo%lld_%llu'>%s</a>", (long long)owner_id,
                                           (unsigned long long)id, photo_text.c_str());
            else
                message.text += str_format("<a href='http://vk.com/photo%lld_%llu'>http://vk.com/photo%lld_%llu</a>",
                                           (long long)owner_id, (unsigned long long)id, (long long)owner_id,
                                           (unsigned long long)id);
            // We append placeholder text, so that we can replace it later in download_thumbnail.
            message.text += str_format("<br><thumbnail-placeholder-%d>", message.thumbnail_urls.size());
            message.thumbnail_urls.push_back(thumbnail);
        } else if (type == "video") {
            if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
                    || !field_is_present<string>(fields, "title") || !field_is_present<string>(fields, "photo_320")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().c_str());
                continue;
            }
            const uint64 id = fields.get("id").get<double>();
            const int64 owner_id = fields.get("owner_id").get<double>();
            const string& title = fields.get("title").get<string>();
            const string& thumbnail = fields.get("photo_320").get<string>();

            message.text += str_format("<a href='http://vk.com/video%lld_%llu'>%s</a>", (long long)owner_id,
                                       (unsigned long long)id, title.c_str());
            // We append placeholder text, so that we can replace it later in download_thumbnail.
            message.text += str_format("<br><thumbnail-placeholder-%d>", message.thumbnail_urls.size());
            message.thumbnail_urls.push_back(thumbnail);
        } else if (type == "audio") {
            if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "artist")
                    || !field_is_present<string>(fields, "title")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().c_str());
                continue;
            }
            const string& url = fields.get("url").get<string>();
            const string& artist = fields.get("artist").get<string>();
            const string& title = fields.get("title").get<string>();

            message.text += str_format("<a href='%s'>%s - %s</a>", url.c_str(), artist.c_str(), title.c_str());
        } else if (type == "doc") {
            if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "title")) {
                purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                                   v.serialize().c_str());
                continue;
            }
            const string& url = fields.get("url").get<string>();
            const string& title = fields.get("title").get<string>();

            message.text += str_format("<a href='%s'>%s</a>", url.c_str(), title.c_str());
        } else {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().c_str());
            message.text += "\nUnknown attachement type ";
            message.text += type;
            continue;
        }
    }
}

void MessageReceiver::download_thumbnail(int message, int thumbnail)
{
    if (message >= int(m_messages.size())) {
        finish();
        return;
    }
    if (thumbnail >= int(m_messages[message].thumbnail_urls.size())) {
        download_thumbnail(message + 1, 0);
        return;
    }

    const string& url = m_messages[message].thumbnail_urls[thumbnail];
    http_get(m_gc, url, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (!purple_http_response_is_successful(response)) {
            purple_debug_error("prpl-vkcom", "Unable to download thumbnail: %s\n",
                               purple_http_response_get_error(response));
            download_thumbnail(message, thumbnail + 1);
            return;
        }

        size_t size;
        const char* data = purple_http_response_get_data(response, &size);
        int img_id = purple_imgstore_add_with_id(g_memdup(data, size), size, nullptr);

        string img_tag = str_format("<img id=\"%d\">", img_id);
        string img_placeholder = str_format("<thumbnail-placeholder-%d>", thumbnail);
        str_replace(m_messages[message].text, img_placeholder, img_tag);

        download_thumbnail(message, thumbnail + 1);
    });
}

void MessageReceiver::finish()
{
    std::sort(m_messages.begin(), m_messages.end(), [](const ReceivedMessage& a, const ReceivedMessage& b) {
        return a.timestamp < b.timestamp;
    });

    uint64_vec message_ids;
    for (const ReceivedMessage& m: m_messages) {
        serv_got_im(m_gc, buddy_name_from_uid(m.uid).c_str(), m.text.c_str(), PURPLE_MESSAGE_RECV, m.timestamp);
        message_ids.push_back(m.mid);
    }
    mark_message_as_read(m_gc, message_ids);

    if (m_received_cb)
        m_received_cb();
    delete this;
}

} // End of anonymous namespace
