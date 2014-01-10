#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <time.h>

#include <debug.h>
#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-message-recv.h"


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

    // Receives all messages starting after last_msg_id.
    void run_all(uint64 last_msg_id);
    // Receives messages with given ids.
    void run(const uint64_vec& message_ids);

private:
    enum MessageStatus {
        MESSAGE_INCOMING_READ,
        MESSAGE_INCOMING_UNREAD,
        MESSAGE_OUTGOING
    };

    struct Message
    {
        uint64 mid;
        uint64 uid;
        uint64 chat_id; // If chat_id is 0, this is a regular IM message.
        string text;
        time_t timestamp;
        MessageStatus status;

        // A list of thumbnail URLs to download and append to message. Set in process_attachments,
        // replaced in download_thumbnails.
        vector<string> thumbnail_urls;
        // A list of user and group ids, present in message. Set in process_attachments
        // and process_fwd_message, replaced in replace_user_ids and replace_group_ids.
        vector<uint64> user_ids;
        vector<uint64> group_ids;
    };
    vector<Message> m_messages;

    PurpleConnection* m_gc;
    ReceivedCb m_received_cb;

    MessageReceiver(PurpleConnection* gc, const ReceivedCb& received_cb)
        : m_gc(gc),
          m_received_cb(received_cb)
    {
    }

    ~MessageReceiver()
    {
    }

    // Runs messages.get from given offset.
    void run_all(uint64 last_msg_id, bool outgoing);
    // Processes one item from the result of messages.get and messages.getById.
    void process_message(const picojson::value& fields);
    // Processes attachments: appends urls to message text, adds thumbnail_urls.
    static void process_attachments(PurpleConnection* gc, const picojson::array& items, Message& message);
    // Processes forwarded messages: appends message text and processes attachments.
    static void process_fwd_message(PurpleConnection* gc, const picojson::value& fields, Message& message);
    // Processes photo attachment.
    static void process_photo_attachment(const picojson::value& items, Message& message);
    // Processes video attachment.
    static void process_video_attachment(const picojson::value& fields, Message& message);
    // Processes audio attachment.
    static void process_audio_attachment(const picojson::value& fields, Message& message);
    // Processes doc attachment.
    static void process_doc_attachment(const picojson::value& fields, Message& message);
    // Processes wall post attachment.
    static void process_wall_attachment(PurpleConnection* gc, const picojson::value& fields, Message& message);
    // Processes link attachment.
    static void process_link_attachment(const picojson::value& fields, Message& message);

    // Appends specific thumbnail placeholder to the end of message text. Placeholder will be replaced
    // by actual image later in download_thumbnail().
    static void append_thumbnail_placeholder(const string& thumbnail_url, Message& message);
    // Returns user/group placeholder, which should be appended to the message text. It will
    // be replaced with actual user/group name and link to the page later in replace_user/group_ids().
    static string get_user_placeholder(PurpleConnection* gc, uint64 user_id, Message& message);
    static string get_group_placeholder(uint64 group_id, Message& message);

    // Downloads the given thumbnail for given message, modifies corresponding message text
    // and calls either next download_thumbnail() or finish(). message is index into m_messages,
    // thumbnail is index into thumbnail_urls.
    void download_thumbnail(size_t message, size_t thumbnail);
    // Replaces all placeholder texts for user/group ids in messages with user/group names
    // and hrefs. Gets information on users, which are not present in user_infos, and groups
    // from vk.com
    void replace_user_ids();
    void replace_group_ids();
    // Sorts received messages, sends them to libpurple client and destroys this.
    void finish();
};

} // End of anonymous namespace

void receive_messages_range(PurpleConnection* gc, uint64 last_msg_id, const ReceivedCb& received_cb)
{
    MessageReceiver* receiver = MessageReceiver::create(gc, received_cb);
    receiver->run_all(last_msg_id);
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

void MessageReceiver::run_all(uint64 last_msg_id)
{
    run_all(last_msg_id, false);
}

void MessageReceiver::run(const uint64_vec& message_ids)
{
    if (message_ids.empty()) {
        if (m_received_cb)
            m_received_cb(0);
        return;
    }

    string ids_str = str_concat_int(',', message_ids);
    CallParams params = { {"message_ids", ids_str} };
    vk_call_api_items(m_gc, "messages.getById", params, false, [=](const picojson::value& message) {
        process_message(message);
    }, [=] {
        download_thumbnail(0, 0);
    }, [=](const picojson::value&) {
        finish();
    });
}

void MessageReceiver::run_all(uint64 last_msg_id, bool outgoing)
{
    CallParams params = { {"out", outgoing ? "1" : "0"}, {"count", "200"} };
    if (last_msg_id == 0) {
        // First-time login, receive only incoming unread messages.
        assert(!outgoing);
        purple_debug_info("prpl-vkcom", "First login, receiving only unread %s messages\n",
                          outgoing ? "outgoing" : "incoming");
        params.emplace_back("filters", "1");
    } else {
        // We've logged in before, let's download all messages since last time, including read ones.
        purple_debug_info("prpl-vkcom", "Receiving %s messages starting from %llu\n",
                          outgoing ? "outgoing" : "incoming", (long long unsigned)last_msg_id + 1);
        params.emplace_back("last_message_id", to_string(last_msg_id));
    }

    vk_call_api_items(m_gc, "messages.get", params, true, [=](const picojson::value& message) {
        process_message(message);
    }, [=] {
        purple_debug_info("prpl-vkcom", "Finished processing %s messages\n", outgoing ? "outgoing" : "incoming");
        if (!outgoing && last_msg_id != 0)
            run_all(last_msg_id, true);
        else
            download_thumbnail(0, 0);
    }, [=](const picojson::value&) {
        finish();
    });
}


// NOTE:
//  * We must escape text, otherwise we cannot receive comment, containing &amp; or <br> as libpurple
//    will wrongfully interpret them as markup.
//  * Links are returned as plaintext, linkified by Pidgin etc.
//  * Smileys are returned as Unicode emoji (both emoji and pseudocode smileys are accepted on message send).
string cleanup_message_body(const string& body)
{
    char* escaped = purple_markup_escape_text(body.data(), -1);
    string text = escaped;
    g_free(escaped);
    replace_emoji_with_text(text);
    return text;
}

// Converts timestamp, received from server, to string in local time.
string timestamp_to_long_format(time_t timestamp)
{
    struct tm tm;
    localtime_r(&timestamp, &tm);
    return purple_date_format_long(&tm);
}

void MessageReceiver::process_message(const picojson::value& fields)
{
    if (!field_is_present<double>(fields, "user_id") || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "body") || !field_is_present<double>(fields, "id")
            || !field_is_present<double>(fields, "read_state")|| !field_is_present<double>(fields, "out")) {
        purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                           fields.serialize().data());
        return;
    }

    m_messages.push_back({});
    Message& message = m_messages.back();
    message.mid = fields.get("id").get<double>();
    message.uid = fields.get("user_id").get<double>();
    message.chat_id = 0;
    if (field_is_present<double>(fields, "chat_id"))
        message.chat_id = fields.get("chat_id").get<double>();

    message.text = cleanup_message_body(fields.get("body").get<string>());
    message.timestamp = fields.get("date").get<double>();
    if (fields.get("out").get<double>() != 0.0)
        message.status = MESSAGE_OUTGOING;
    else if (fields.get("read_state").get<double>() == 0.0)
        message.status = MESSAGE_INCOMING_UNREAD;
    else
        message.status = MESSAGE_INCOMING_READ;

    // Process attachments: append information to text.
    if (field_is_present<picojson::array>(fields, "attachments"))
        process_attachments(m_gc, fields.get("attachments").get<picojson::array>(), message);

    // Process forwarded messages.
    if (field_is_present<picojson::array>(fields, "fwd_messages")) {
        const picojson::array& fwd_messages = fields.get("fwd_messages").get<picojson::array>();
        for (const picojson::value& m: fwd_messages)
            process_fwd_message(m_gc, m, message);
    }
}

void MessageReceiver::process_attachments(PurpleConnection* gc, const picojson::array& items, Message& message)
{
    for (const picojson::value& v: items) {
        if (!field_is_present<string>(v, "type")) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            return;
        }
        const string& type = v.get("type").get<string>();
        if (!field_is_present<picojson::object>(v, type)) {
            purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            return;
        }
        const picojson::value& fields = v.get(type);

        if (!message.text.empty())
            message.text += "<br>";

        if (type == "photo") {
            process_photo_attachment(fields, message);
        } else if (type == "video") {
            process_video_attachment(fields, message);
        } else if (type == "audio") {
            process_audio_attachment(fields, message);
        } else if (type == "doc") {
            process_doc_attachment(fields, message);
        } else if (type == "wall") {
            process_wall_attachment(gc, fields, message);
        } else if (type == "link") {
            process_link_attachment(fields, message);
        } else {
            purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                               "or messages.getById: type %s, %s\n", type.data(), fields.serialize().data());
            message.text += "\nUnknown attachement type ";
            message.text += type;
            continue;
        }
    }
}

void MessageReceiver::process_fwd_message(PurpleConnection* gc, const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "user_id") || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "body")) {
        purple_debug_error("prpl-vkcom", "Strange response from messages.get or messages.getById: %s\n",
                           fields.serialize().data());
        return;
    }

    message.text += "<br>";

    uint64 user_id = fields.get("user_id").get<double>();
    string date = timestamp_to_long_format(fields.get("date").get<double>());
    // Placeholder will be replaced with proper name and href in replace_ids().
    string text = str_format("Forwarded message (from %s on %s):\n",
                             get_user_placeholder(gc, user_id, message).data(), date.data());
    text += cleanup_message_body(fields.get("body").get<string>());
    // Prepend quotation marks to all forwared message lines.
    str_replace(text, "\n", "\n    > ");

    message.text += text;

    message.user_ids.push_back(user_id);

    if (field_is_present<picojson::array>(fields, "attachments"))
        process_attachments(gc, fields.get("attachments").get<picojson::array>(), message);
}

void MessageReceiver::process_photo_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
            || !field_is_present<string>(fields, "text") || !field_is_present<string>(fields, "photo_604")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const uint64 id = fields.get("id").get<double>();
    const int64 owner_id = fields.get("owner_id").get<double>();
    const string& photo_text = fields.get("text").get<string>();
    const string& thumbnail = fields.get("photo_604").get<string>();

    // Apparently, there is no URL for private photos (such as the one for docs:
    // http://vk.com/docXXX_XXX?hash="access_key". If we've got "access_key" as a parameter, it means
    // that the photo is private, so we should rather link to the biggest version of the photo.
    string url;
    if (field_is_present<string>(fields, "access_key")) {
        // We have to find the max photo URL, as we do not always receive all sizes.
        if (field_is_present<string>(fields, "photo_2560"))
            url = fields.get("photo_2560").get<string>();
        else if (field_is_present<string>(fields, "photo_1280"))
            url = fields.get("photo_1280").get<string>();
        else if (field_is_present<string>(fields, "photo_807"))
            url = fields.get("photo_807").get<string>();
        else
            url = thumbnail;
    } else {
        url = str_format("http://vk.com/photo%lld_%llu", (long long)owner_id, (unsigned long long)id);
    }

    if (!photo_text.empty())
        message.text += str_format("<a href='%s'>%s</a>", url.data(), photo_text.data());
    else
        message.text += str_format("<a href='%s'>%s</a>", url.data(), url.data());
    append_thumbnail_placeholder(thumbnail, message);
}

void MessageReceiver::process_video_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
            || !field_is_present<string>(fields, "title") || !field_is_present<string>(fields, "photo_320")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const uint64 id = fields.get("id").get<double>();
    const int64 owner_id = fields.get("owner_id").get<double>();
    const string& title = fields.get("title").get<string>();
    const string& thumbnail = fields.get("photo_320").get<string>();

    message.text += str_format("<a href='http://vk.com/video%lld_%llu'>%s</a>", (long long)owner_id,
                               (unsigned long long)id, title.data());

    append_thumbnail_placeholder(thumbnail, message);
}

void MessageReceiver::process_audio_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "artist")
            || !field_is_present<string>(fields, "title")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& url = fields.get("url").get<string>();
    const string& artist = fields.get("artist").get<string>();
    const string& title = fields.get("title").get<string>();

    message.text += str_format("<a href='%s'>%s - %s</a>", url.data(), artist.data(), title.data());
}

void MessageReceiver::process_doc_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "title")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& url = fields.get("url").get<string>();
    const string& title = fields.get("title").get<string>();

    message.text += str_format("<a href='%s'>%s</a>", url.data(), title.data());

    // Check if we've got a thumbnail.
    if (field_is_present<string>(fields, "photo_130")) {
        const string& thumbnail = fields.get("photo_130").get<string>();
        append_thumbnail_placeholder(thumbnail, message);
    }
}

void MessageReceiver::process_wall_attachment(PurpleConnection* gc, const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id")
            || (!field_is_present<double>(fields, "to_id") && !field_is_present<double>(fields, "from_id"))
            || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "text")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }

    message.text += "<br>";

    uint64 id = fields.get("id").get<double>();
    // This happens in case of reposts, where only "from_id" is specified.
    int64 to_id;
    if (field_is_present<double>(fields, "to_id"))
        to_id = fields.get("to_id").get<double>();
    else
        to_id = fields.get("from_id").get<double>();

    if (to_id > 0) {
        message.text += get_user_placeholder(gc, to_id, message);
    } else {
        message.text += get_group_placeholder(-to_id, message);
    }

    string wall_url = str_format("http://vk.com/wall%" PRIi64 "_%" PRIu64, to_id, id);
    const char* verb = (fields.contains("copy_text") || fields.contains("copy_history"))
                        ? "reposted" : "posted";
    string date = timestamp_to_long_format(fields.get("date").get<double>());

    message.text += str_format(" <a href='%s'>%s</a> on %s<br>", wall_url.data(), verb, date.data());

    if (field_is_present<string>(fields, "copy_text")) {
        message.text += fields.get("copy_text").get<string>();
        message.text += "<br>";
    }
    message.text += fields.get("text").get<string>();

    if (field_is_present<picojson::array>(fields, "attachments"))
        process_attachments(gc, fields.get("attachments").get<picojson::array>(), message);

    if (field_is_present<picojson::array>(fields, "copy_history")) {
        const picojson::array& a = fields.get("copy_history").get<picojson::array>();
        for (const picojson::value& v: a)
            process_wall_attachment(gc, v, message);
    }
}

void MessageReceiver::process_link_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url")) {
        purple_debug_error("prpl-vkcom", "Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& url = fields.get("url").get<string>();

    // link attachment is not described anywhere in Vk.com documentation, so we cannot be sure,
    // which fields are required and which are optional. Let's treat all of them apart from url
    // as optional.
    string title;
    if (field_is_present<string>(fields, "title"))
        title = fields.get("title").get<string>();
    string description;
    if (field_is_present<string>(fields, "description"))
        description = fields.get("description").get<string>();
    string image_src;
    if (field_is_present<string>(fields, "image_src"))
        image_src = fields.get("image_src").get<string>();

    if (!title.empty())
        message.text += str_format("<a href='%s'>%s</a>", url.data(), title.data());
    else
        message.text += url.data();

    if (!description.empty()) {
        message.text += "<br>";
        message.text += description;
    }

    if (!image_src.empty())
        append_thumbnail_placeholder(image_src, message);
}

void MessageReceiver::append_thumbnail_placeholder(const string& thumbnail_url, Message& message)
{
    // TODO: If the conversation is open and an outgoing message has been received, we should show
    // the image too.
    if (message.status == MESSAGE_INCOMING_UNREAD) {
        message.text += str_format("<br><thumbnail-placeholder-%d>", message.thumbnail_urls.size());
        message.thumbnail_urls.push_back(thumbnail_url);
    }
}

string MessageReceiver::get_user_placeholder(PurpleConnection* gc, uint64 user_id, Message& message)
{
    VkUserInfo* info = get_user_info_for_buddy(gc, user_id);
    if (info)
        return get_user_href(user_id, *info);

    string text = str_format("<user-placeholder-%d>", message.user_ids.size());
    message.user_ids.push_back(user_id);
    return text;
}

string MessageReceiver::get_group_placeholder(uint64 group_id, Message& message)
{
    string text = str_format("<group-placeholder-%d>", message.group_ids.size());
    message.group_ids.push_back(group_id);
    return text;
}

void MessageReceiver::download_thumbnail(size_t message, size_t thumbnail)
{
    if (message >= m_messages.size()) {
        replace_user_ids();
        return;
    }
    if (thumbnail >= m_messages[message].thumbnail_urls.size()) {
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

void MessageReceiver::replace_user_ids()
{
    // Get all uids, which are not present in user_infos.
    uint64_vec unknown_uids;
    for (const Message& m: m_messages)
        for (uint64 id: m.user_ids)
            unknown_uids.push_back(id);

    add_or_update_user_infos(m_gc, unknown_uids, [=] {
        for (Message& m: m_messages) {
            for (unsigned i = 0; i < m.user_ids.size(); i++) {
                uint64 id = m.user_ids[i];
                string placeholder = str_format("<user-placeholder-%d>", i);
                VkUserInfo* info = get_user_info_for_buddy(m_gc, id);
                string href = get_user_href(id, *info);
                str_replace(m.text, placeholder, href);
            }
        }

        replace_group_ids();
    });
}

void MessageReceiver::replace_group_ids()
{
    uint64_vec group_ids;
    for (const Message& m: m_messages)
        for (uint64 id: m.group_ids)
            group_ids.push_back(id);

    get_groups_info(m_gc, group_ids, [=](const map<uint64, VkGroupInfo>& infos) {
        for (Message& m: m_messages) {
            for (unsigned i = 0; i < m.group_ids.size(); i++) {
                uint64 group_id = m.group_ids[i];
                string placeholder = str_format("<group-placeholder-%d>", i);
                string href = get_group_href(group_id, infos.at(group_id));
                str_replace(m.text, placeholder, href);
            }
        }

        finish();
    });
}


void MessageReceiver::finish()
{
    std::sort(m_messages.begin(), m_messages.end(), [](const Message& a, const Message& b) {
        return a.mid < b.mid;
    });

    uint64_vec uids_to_user_info; // Users to get information about.
    for (const Message& m: m_messages)
        if (m.status != MESSAGE_OUTGOING && is_unknown_uid(m_gc, m.uid))
            uids_to_user_info.push_back(m.uid);

    add_or_update_user_infos(m_gc, uids_to_user_info, [=] {
        uint64_vec uids_to_buddy_list; // Users to be added to buddy list.
        for (const Message& m: m_messages)
            // We want to add buddies to buddy list for unread personal messages because we will open conversations
            // with them.
            if (m.status == MESSAGE_INCOMING_UNREAD && m.chat_id == 0 && !in_buddy_list(m_gc, m.uid))
                uids_to_buddy_list.push_back(m.uid);

        // We are setting presence because this is the first time we update the buddies.
        add_to_buddy_list(m_gc, uids_to_buddy_list, [=] {
            PurpleLogCache logs(m_gc);
            for (const Message& m: m_messages) {
                if (m.status == MESSAGE_INCOMING_UNREAD) {
                    // Open new conversation for received message.
                    if (m.chat_id == 0) {
                        serv_got_im(m_gc, buddy_name_from_uid(m.uid).data(), m.text.data(), PURPLE_MESSAGE_RECV,
                                    m.timestamp);
                    } else {
//                        TODO: open chat
//                        serv_got_chat_in(m_gc, m.chat_id, get_buddy_name(m_gc, m.uid).data(),
//                                         PURPLE_MESSAGE_RECV, m.text.data(), m.timestamp);
                    }
                } else { // m.status == MESSAGE_INCOMING_READ || m.status == MESSAGE_OUTGOING
                    // Check if the conversation is open, so that we write to the conversation, not the log.
                    // TODO: Remove code duplication with vk-longpoll.cpp
                    string from;
                    PurpleMessageFlags flags;
                    if (m.status == MESSAGE_INCOMING_UNREAD) {
                        from = get_buddy_name(m_gc, m.uid);
                        flags = PURPLE_MESSAGE_RECV;
                    } else {
                        from = purple_account_get_name_for_display(purple_connection_get_account(m_gc));
                        flags = PURPLE_MESSAGE_SEND;
                    }

                    PurpleConversation* conv = find_conv_for_id(m_gc, m.uid, m.chat_id);
                    if (conv) {
                        purple_conv_im_write(PURPLE_CONV_IM(conv), from.data(), m.text.data(), flags,
                                             m.timestamp);
                    } else {
                        PurpleLog* log = (m.chat_id == 0) ? logs.for_uid(m.uid) : logs.for_chat(m.chat_id);
                        purple_log_write(log, flags, from.data(), m.timestamp, m.text.data());
                    }
                }
            }

            // Mark incoming messages as read.
            uint64_vec unread_message_ids;
            for (const Message& m: m_messages)
                if (m.status == MESSAGE_INCOMING_UNREAD)
                    unread_message_ids.push_back(m.mid);
            mark_message_as_read(m_gc, unread_message_ids);

            // Sets the last message id as m_messages are sorted by mid.
            uint64 max_msg_id = 0;
            if (!m_messages.empty())
                max_msg_id = m_messages.back().mid;

            if (m_received_cb)
                m_received_cb(max_msg_id);
            delete this;
        });
    });
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
