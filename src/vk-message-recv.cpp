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
#include "vk-chat.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-message-recv.h"


namespace
{

// The amount of messages to synchronize when logging in for the first time.
const uint MAX_MESSAGES_ON_FIRST_TIME = 5000;

// Function, which returns the last message id, which the user received. It is used to calculate
// message id, which we start receiving messages from.
typedef function_ptr<void(uint64 msg_id)> LastMessageIdCb;
void get_last_message_id(PurpleConnection* gc, LastMessageIdCb last_message_id_cb);

enum MessageStatus {
    MESSAGE_INCOMING_READ,
    MESSAGE_INCOMING_UNREAD,
    MESSAGE_OUTGOING
};

// A structure, describing one received message.
struct Message
{
    uint64 mid;
    uint64 user_id;
    uint64 chat_id; // If chat_id is 0, this is a regular instant message.
    string text;
    time_t timestamp;
    MessageStatus status;

    // A list of thumbnail URLs to download and append to message. Set in process_attachments,
    // replaced in download_thumbnails.
    vector<string> thumbnail_urls;
    // A list of user and group ids, used in the message. Set in process_attachments
    // and process_fwd_message, replaced in replace_user_ids and replace_group_ids.
    vector<uint64> unknown_user_ids;
    vector<uint64> unknown_group_ids;
};

// A structure, capturing all information about received messages.
struct MessagesData
{
    PurpleConnection* gc;
    ReceivedCb received_cb;

    vector<Message> messages;
};
typedef shared_ptr<MessagesData> MessagesData_ptr;

// Receives all messages starting after last_msg_id.
void receive_messages_range_internal(const MessagesData_ptr& data, uint64 last_msg_id, bool outgoing);

// Processes one item from the result of messages.get and messages.getById.
void process_message(const MessagesData_ptr& data, const picojson::value& fields);
// Processes attachments: appends urls to message text, adds thumbnail_urls.
void process_attachments(PurpleConnection* gc, const picojson::array& items, Message& message);
// Processes forwarded messages: appends message text and processes attachments.
void process_fwd_message(PurpleConnection* gc, const picojson::value& fields, Message& message);
// Processes photo attachment.
void process_photo_attachment(const picojson::value& fields, Message& message);
// Processes video attachment.
void process_video_attachment(const picojson::value& fields, Message& message);
// Processes audio attachment.
void process_audio_attachment(const picojson::value& fields, Message& message);
// Processes doc attachment.
void process_doc_attachment(const picojson::value& fields, Message& message);
// Processes wall post attachment.
void process_wall_attachment(PurpleConnection* gc, const picojson::value& fields, Message& message);
// Processes link attachment.
void process_link_attachment(const picojson::value& fields, Message& message);
// Processes album attachment.
void process_album_attachment(const picojson::value& fields, Message& message);
// Processes sticker attachment.
void process_sticker_attachment(const picojson::value& fields, Message& message);

// Appends specific thumbnail placeholder to the end of message text. Placeholder will be replaced
// by actual image later in download_thumbnail(). If prepend_br is false, <br> is prepended only
// when message text is not empty.
void append_thumbnail_placeholder(const string& thumbnail_url, Message& message, bool prepend_br = true);
// Returns user/group placeholder, which should be appended to the message text. It will
// be replaced with actual user/group name and link to the page later in replace_user/group_ids().
string get_user_placeholder(PurpleConnection* gc, uint64 user_id, Message& message);
string get_group_placeholder(PurpleConnection* gc, uint64 group_id, Message& message);

// Downloads the given thumbnail for given message, modifies corresponding message text
// and calls either next download_thumbnail() or finish(). message is index into m_messages,
// thumbnail is index into thumbnail_urls.
void download_thumbnail(const MessagesData_ptr& data, size_t msg_num, size_t thumb_num);
// Replaces all placeholder texts for user/group ids in messages with user/group names
// and hrefs. Gets information on users, which are not present in user_infos, and groups
// from vk.com
void replace_user_ids(const MessagesData_ptr& data);
void replace_group_ids(const MessagesData_ptr& data);
// Adds all users and groups which are senders/receiveirs of message (needed to get their names/open
// conversation with them).
void add_unknown_users_chats(const MessagesData_ptr& data);
// Sorts received messages, sends them to libpurple client and destroys this.
void finish_receiving(const MessagesData_ptr& data);

} // End of anonymous namespace

void receive_messages_range(PurpleConnection* gc, uint64 last_msg_id, const ReceivedCb& received_cb)
{
    MessagesData_ptr data{ new MessagesData };
    data->gc = gc;
    data->received_cb = received_cb;

    if (last_msg_id == 0) {
        // The user has logged in from this computer for the first time. Do not download the
        // whole history, but get the last message id and download no more than MAX_MESSAGES_ON_FIRST_TIME
        // before it.
        get_last_message_id(gc, [=](uint64 real_last_msg_id) {
            uint64 start_msg_id = 0;
            if (real_last_msg_id > MAX_MESSAGES_ON_FIRST_TIME)
                start_msg_id = real_last_msg_id - MAX_MESSAGES_ON_FIRST_TIME;
            receive_messages_range_internal(data, start_msg_id, false);
        });
    } else {
        receive_messages_range_internal(data, last_msg_id, false);
    }
}

namespace
{

void receive_messages_impl(PurpleConnection* gc, const vector<uint64>& message_ids, size_t offset)
{
    if (message_ids.empty())
        return;

    MessagesData_ptr data{ new MessagesData() };
    data->gc = gc;
    data->received_cb = nullptr;

    size_t num = max_urlencoded_int(message_ids.data() + offset, message_ids.data() + message_ids.size());
    string ids_str = str_concat_int(',', message_ids.begin() + offset, message_ids.begin() + offset + num);
    CallParams params = { {"message_ids", ids_str} };
    vk_call_api_items(data->gc, "messages.getById", params, false, [=](const picojson::value& message) {
        process_message(data, message);
    }, [=] {
        download_thumbnail(data, 0, 0);
    }, [=](const picojson::value&) {
        finish_receiving(data);

        size_t next_offset = offset + num;
        if (next_offset < message_ids.size())
            receive_messages_impl(gc, message_ids, next_offset);
    });
}

}

void receive_messages(PurpleConnection* gc, const vector<uint64>& message_ids)
{
    receive_messages_impl(gc, message_ids, 0);
}

namespace
{

void get_last_message_id(PurpleConnection* gc, LastMessageIdCb last_message_id_cb)
{
    CallParams params = { {"code", "return API.messages.get({\"count\": 1}).items[0].id;" } };
    vk_call_api(gc, "execute", params, [=](const picojson::value& v) {
        if (!v.is<double>()) {
            vkcom_debug_error("Strange response from messages.get: %s\n",
                               v.serialize().data());
            last_message_id_cb(0);
            return;
        }

        last_message_id_cb(v.get<double>());
    }, [=](const picojson::value&) {
        last_message_id_cb(0);
    });
}

void receive_messages_range_internal(const MessagesData_ptr& data, uint64 last_msg_id, bool outgoing)
{
    vkcom_debug_info("Receiving %s messages starting from %" PRIu64 "\n",
                      outgoing ? "outgoing" : "incoming", last_msg_id + 1);

    CallParams params = { {"out", outgoing ? "1" : "0"}, {"count", "200"},
                          {"last_message_id", to_string(last_msg_id) } };

    vk_call_api_items(data->gc, "messages.get", params, true, [=](const picojson::value& message) {
        process_message(data, message);
    }, [=] {
        vkcom_debug_info("Finished processing %s messages\n", outgoing ? "outgoing" : "incoming");
        if (!outgoing)
            receive_messages_range_internal(data, last_msg_id, true);
        else
            download_thumbnail(data, 0, 0);
    }, [=](const picojson::value&) {
        finish_receiving(data);
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
    return purple_date_format_long(localtime(&timestamp));
}

void process_message(const MessagesData_ptr& data, const picojson::value& fields)
{
    if (!field_is_present<double>(fields, "user_id") || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "body") || !field_is_present<double>(fields, "id")
            || !field_is_present<double>(fields, "read_state")|| !field_is_present<double>(fields, "out")) {
        vkcom_debug_error("Strange response from messages.get or messages.getById: %s\n",
                           fields.serialize().data());
        return;
    }

    Message message;
    message.mid = fields.get("id").get<double>();
    message.user_id = fields.get("user_id").get<double>();
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
        process_attachments(data->gc, fields.get("attachments").get<picojson::array>(), message);

    // Process forwarded messages.
    if (field_is_present<picojson::array>(fields, "fwd_messages")) {
        const picojson::array& fwd_messages = fields.get("fwd_messages").get<picojson::array>();
        for (const picojson::value& m: fwd_messages)
            process_fwd_message(data->gc, m, message);
    }
    data->messages.push_back(std::move(message));
}

void process_attachments(PurpleConnection* gc, const picojson::array& items, Message& message)
{
    for (const picojson::value& v: items) {
        if (!field_is_present<string>(v, "type")) {
            vkcom_debug_error("Strange response from messages.get or messages.getById: %s\n",
                               v.serialize().data());
            return;
        }
        const string& type = v.get("type").get<string>();
        if (!field_is_present<picojson::object>(v, type)) {
            vkcom_debug_error("Strange response from messages.get or messages.getById: %s\n",
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
        } else if (type == "album") {
            process_album_attachment(fields, message);
        } else if (type == "sticker") {
            process_sticker_attachment(fields, message);
        } else {
            vkcom_debug_error("Strange attachment in response from messages.get "
                               "or messages.getById: type %s, %s\n", type.data(), fields.serialize().data());
            message.text += "\nUnknown attachement type ";
            message.text += type;
            continue;
        }
    }
}

void process_fwd_message(PurpleConnection* gc, const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "user_id") || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "body")) {
        vkcom_debug_error("Strange response from messages.get or messages.getById: %s\n",
                           fields.serialize().data());
        return;
    }

    message.text += "<br>";

    uint64 user_id = fields.get("user_id").get<double>();
    string date = timestamp_to_long_format(fields.get("date").get<double>());
    // Placeholder either contains a formed href, if the user is already known, or will be replaced
    // with proper name and href in replace_ids().
    string text = str_format("Forwarded message (from %s on %s):\n",
                             get_user_placeholder(gc, user_id, message).data(), date.data());
    text += cleanup_message_body(fields.get("body").get<string>());
    // Prepend quotation marks to all forwared message lines.
    str_replace(text, "\n", "\n    > ");

    message.text += text;

    if (field_is_present<picojson::array>(fields, "attachments"))
        process_attachments(gc, fields.get("attachments").get<picojson::array>(), message);
}

void process_photo_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
            || !field_is_present<string>(fields, "text") || !field_is_present<string>(fields, "photo_604")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const uint64 id = fields.get("id").get<double>();
    const int64 owner_id = fields.get("owner_id").get<double>();
    const string& photo_text = fields.get("text").get<string>();
    const string& thumbnail = fields.get("photo_604").get<string>();

    // Apparently, there is no URL for private photos (such as the one for docs:
    // https://vk.com/docXXX_XXX?hash="access_key". If we've got "access_key" as a parameter, it means
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
        url = str_format("https://vk.com/photo%" PRId64 "_%" PRIu64, owner_id, id);
    }

    if (!photo_text.empty())
        message.text += str_format("<a href='%s'>%s</a>", url.data(), photo_text.data());
    else
        message.text += str_format("<a href='%s'>%s</a>", url.data(), url.data());
    append_thumbnail_placeholder(thumbnail, message);
}

void process_video_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id") || !field_is_present<double>(fields, "owner_id")
            || !field_is_present<string>(fields, "title") || !field_is_present<string>(fields, "photo_320")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const uint64 id = fields.get("id").get<double>();
    const int64 owner_id = fields.get("owner_id").get<double>();
    const string& title = fields.get("title").get<string>();
    const string& thumbnail = fields.get("photo_320").get<string>();

    message.text += str_format("<a href='https://vk.com/video%" PRId64 "_%" PRIu64 "'>%s</a>", owner_id,
                               id, title.data());

    append_thumbnail_placeholder(thumbnail, message);
}

void process_audio_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "artist")
            || !field_is_present<string>(fields, "title")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& url = fields.get("url").get<string>();
    const string& artist = fields.get("artist").get<string>();
    const string& title = fields.get("title").get<string>();

    message.text += str_format("<a href='%s'>%s - %s</a>", url.data(), artist.data(), title.data());
}

void process_doc_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url") || !field_is_present<string>(fields, "title")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
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

void process_wall_attachment(PurpleConnection* gc, const picojson::value& fields, Message& message)
{
    if (!field_is_present<double>(fields, "id")
            || (!field_is_present<double>(fields, "to_id") && !field_is_present<double>(fields, "from_id"))
            || !field_is_present<double>(fields, "date")
            || !field_is_present<string>(fields, "text")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
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
        message.text += get_group_placeholder(gc, -to_id, message);
    }

    string wall_url = str_format("https://vk.com/wall%" PRIi64 "_%" PRIu64, to_id, id);
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

void process_link_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "url")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
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

void process_album_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "id") || !field_is_present<double>(fields, "owner_id")
            || !field_is_present<string>(fields, "title")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& id = fields.get("id").get<string>();
    string owner_id = fields.get("owner_id").to_str();
    const string& title = fields.get("title").get<string>();

    string url = str_format("https://vk.com/album%s_%s", owner_id.data(), id.data());

    message.text += str_format("Album: <a href='%s'>%s</a>", url.data(), title.data());
}

void process_sticker_attachment(const picojson::value& fields, Message& message)
{
    if (!field_is_present<string>(fields, "photo_64")) {
        vkcom_debug_error("Strange attachment in response from messages.get "
                           "or messages.getById: %s\n", fields.serialize().data());
        return;
    }
    const string& thumbnail = fields.get("photo_64").get<string>();

    append_thumbnail_placeholder(thumbnail, message, false);
}

void append_thumbnail_placeholder(const string& thumbnail_url, Message& message, bool prepend_br)
{
    // TODO: If the conversation is open and an outgoing message has been received, we should show
    // the image too.
    if (message.status == MESSAGE_INCOMING_UNREAD) {
        // We will download the image later and replace the placeholder.
        if (!message.text.empty() || prepend_br)
            message.text += "<br>";
        message.text += str_format("<thumbnail-placeholder-%d>", message.thumbnail_urls.size());
        message.thumbnail_urls.push_back(thumbnail_url);
    } else {
        // Pidgin does not store images in logs, store the URL itself. so that the information
        // does not get lost.
        message.text += str_format("%s", thumbnail_url.data());
    }
}

string get_user_placeholder(PurpleConnection* gc, uint64 user_id, Message& message)
{
    if (user_id == 0)
        return "";

    VkUserInfo* info = get_user_info(gc, user_id);
    // We can have user_info, but the user can be unknown.
    if (info && !is_unknown_user(gc, user_id)) {
        return get_user_href(user_id, *info);
    } else {
        // We will get user information later and replace the placeholder.
        string text = str_format("<user-placeholder-%d>", message.unknown_user_ids.size());
        message.unknown_user_ids.push_back(user_id);
        return text;
    }
}

string get_group_placeholder(PurpleConnection* gc, uint64 group_id, Message& message)
{
    if (group_id == 0)
        return "";

    VkGroupInfo* info = get_group_info(gc, group_id);
    // We can have group_info, but the group can be unknown.
    if (info && !is_unknown_group(gc, group_id)) {
        return get_group_href(group_id, *info);
    } else {
        // We will get group information later and replace the placeholder.
        string text = str_format("<group-placeholder-%d>", message.unknown_group_ids.size());
        message.unknown_group_ids.push_back(group_id);
        return text;
    }
}

void download_thumbnail(const MessagesData_ptr& data, size_t msg_num, size_t thumb_num)
{
    if (msg_num >= data->messages.size()) {
        replace_user_ids(data);
        return;
    }
    if (thumb_num >= data->messages[msg_num].thumbnail_urls.size()) {
        download_thumbnail(data, msg_num + 1, 0);
        return;
    }

    const string& url = data->messages[msg_num].thumbnail_urls[thumb_num];
    http_get(data->gc, url, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (!purple_http_response_is_successful(response)) {
            vkcom_debug_error("Unable to download thumbnail: %s\n",
                               purple_http_response_get_error(response));
            download_thumbnail(data, msg_num, thumb_num + 1);
            return;
        }

        size_t size;
        const char* img_data = purple_http_response_get_data(response, &size);
        int img_id = purple_imgstore_add_with_id(g_memdup(img_data, size), size, nullptr);

        string img_tag = str_format("<img id=\"%d\">", img_id);
        string img_placeholder = str_format("<thumbnail-placeholder-%d>", thumb_num);
        str_replace(data->messages[msg_num].text, img_placeholder, img_tag);

        download_thumbnail(data, msg_num, thumb_num + 1);
    });
}

void replace_user_ids(const MessagesData_ptr& data)
{
    // Get all user ids, which are not present in user_infos.
    set<uint64> unknown_user_ids;
    for (const Message& message: data->messages) {
        insert_if(unknown_user_ids, message.unknown_user_ids, [=](uint64 user_id) {
            return is_unknown_user(data->gc, user_id);
        });
    }

    update_user_infos(data->gc, unknown_user_ids, [=] {
        for (Message& m: data->messages) {
            for (unsigned i = 0; i < m.unknown_user_ids.size(); i++) {
                uint64 id = m.unknown_user_ids[i];
                VkUserInfo* info = get_user_info(data->gc, id);
                // Getting the user info could fail.
                if (!info)
                    continue;

                string placeholder = str_format("<user-placeholder-%d>", i);
                string href = get_user_href(id, *info);
                str_replace(m.text, placeholder, href);
            }
        }

        replace_group_ids(data);
    });
}

void replace_group_ids(const MessagesData_ptr& data)
{
    vector<uint64> group_ids;
    for (const Message& m: data->messages) {
        append_if(group_ids, m.unknown_group_ids, [=](uint64 group_id) {
            return is_unknown_group(data->gc, group_id);
        });
    }

    update_groups_info(data->gc, group_ids, [=] {
        for (Message& m: data->messages) {
            for (unsigned i = 0; i < m.unknown_group_ids.size(); i++) {
                uint64 group_id = m.unknown_group_ids[i];
                VkGroupInfo* info = get_group_info(data->gc, group_id);
                // Getting the group info could fail.
                if (!info)
                    continue;

                string placeholder = str_format("<group-placeholder-%d>", i);
                string href = get_group_href(group_id, *info);
                str_replace(m.text, placeholder, href);
            }
        }

        add_unknown_users_chats(data);
    });
}

void add_unknown_users_chats(const MessagesData_ptr& data)
{
    // Chats to get information about: all incoming chats. Chat participants are updated
    // when updating chat information.
    set<uint64> unknown_chat_ids;
    for (const Message& m: data->messages)
        if (m.status != MESSAGE_OUTGOING && m.chat_id != 0 && is_unknown_chat(data->gc, m.chat_id))
            unknown_chat_ids.insert(m.chat_id);

    update_chat_infos(data->gc, unknown_chat_ids, [=] {
        // Chats to be added to buddy list: all unread incoming chats.
        set<uint64> chat_ids_to_buddy_list;
        for (const Message& m: data->messages)
            if (m.status == MESSAGE_INCOMING_UNREAD && m.chat_id != 0 && !chat_in_buddy_list(data->gc, m.chat_id))
                chat_ids_to_buddy_list.insert(m.chat_id);

        add_chats_if_needed(data->gc, chat_ids_to_buddy_list, [=] {
            // Users to get information about: authors of every read incoming message (we need their real
            // names when we write to the log) and all the users to be added to the buddy list (see below).
            // Not all chat participants have been updated when chat information was updated due to chats
            // being deactivated, so let's update info for them.
            set<uint64> unknown_user_ids;
            for (const Message& m: data->messages)
                if (m.status != MESSAGE_OUTGOING && is_unknown_user(data->gc, m.user_id))
                    unknown_user_ids.insert(m.user_id);

            update_user_infos(data->gc, unknown_user_ids, [=] {
                // Users to be added to buddy list: authors of unread non-chat messages. Chat participants
                // are never added to buddy list (unless they are already there).
                set<uint64> user_ids_to_buddy_list;
                for (const Message& m: data->messages)
                    if (m.status == MESSAGE_INCOMING_UNREAD && m.chat_id == 0 && !user_in_buddy_list(data->gc, m.user_id))
                        user_ids_to_buddy_list.insert(m.user_id);

                add_buddies_if_needed(data->gc, user_ids_to_buddy_list, [=] {
                    finish_receiving(data);
                });
            });
        });
    });
}

void finish_receiving(const MessagesData_ptr& data)
{
    std::sort(data->messages.begin(), data->messages.end(), [](const Message& a, const Message& b) {
        return a.mid < b.mid;
    });

    // We could've received duplicate messages if a new message arrived between asking for
    // two message batches (with two different offsets). In this case batches overlap (offset
    // starts counting from other base).
    unique(data->messages, [](const Message& a, const Message& b) {
        return a.mid == b.mid;
    });

    PurpleLogCache logs(data->gc);
    for (const Message& m: data->messages) {
        if (m.status == MESSAGE_INCOMING_UNREAD) {
            // Open new conversation for received message.
            if (m.chat_id == 0) {
                string from = user_name_from_id(m.user_id);
                serv_got_im(data->gc, from.data(), m.text.data(), PURPLE_MESSAGE_RECV, m.timestamp);
            } else {
                // Ideally, the chat info would be already added, so the lambda will be called in the current
                // context.
                open_chat_conv(data->gc, m.chat_id, [=] {
                    int conv_id = chat_id_to_conv_id(data->gc, m.chat_id);
                    string from = get_user_display_name(data->gc, m.user_id, m.chat_id);
                    serv_got_chat_in(data->gc, conv_id, from.data(), PURPLE_MESSAGE_RECV, m.text.data(),
                                     m.timestamp);
                });
            }
        } else { // m.status == MESSAGE_INCOMING_READ || m.status == MESSAGE_OUTGOING
            // Check if the conversation is open, so that we write to the conversation, not the log.
            // TODO: Remove code duplication with vk-longpoll.cpp
            string from;
            PurpleMessageFlags flags;
            if (m.status == MESSAGE_INCOMING_READ) {
                if (m.chat_id != 0)
                    from = get_user_display_name(data->gc, m.user_id, m.chat_id);
                else
                    from = get_user_display_name(data->gc, m.user_id);
                flags = PURPLE_MESSAGE_RECV;
            } else {
                if (m.chat_id != 0)
                    from = get_self_chat_display_name(data->gc);
                else
                    from = purple_account_get_name_for_display(purple_connection_get_account(data->gc));
                flags = PURPLE_MESSAGE_SEND;
            }

            PurpleConversation* conv = find_conv_for_id(data->gc, m.user_id, m.chat_id);
            if (conv) {
                if (m.chat_id == 0)
                    // It is possible to use real name as the second parameter instead of username
                    // in the form of "idXXX".
                    purple_conv_im_write(PURPLE_CONV_IM(conv), from.data(), m.text.data(), flags,
                                         m.timestamp);
                else
                    purple_conv_chat_write(PURPLE_CONV_CHAT(conv), from.data(), m.text.data(), flags,
                                           m.timestamp);
            } else {
                PurpleLog* log;
                if (m.chat_id == 0)
                    log = logs.for_user(m.user_id);
                else
                    log = logs.for_chat(m.chat_id);
                purple_log_write(log, flags, from.data(), m.timestamp, m.text.data());
            }
        }
    }

    // Mark incoming messages as read.
    vector<VkReceivedMessage> unread_messages;
    for (const Message& m: data->messages)
        if (m.status == MESSAGE_INCOMING_UNREAD)
            unread_messages.push_back(VkReceivedMessage{ m.mid, m.user_id, m.chat_id });
    mark_message_as_read(data->gc, unread_messages);

    // Sets the last message id as m_messages are sorted by mid.
    uint64 max_msg_id = 0;
    if (!data->messages.empty())
        max_msg_id = data->messages.back().mid;

    if (data->received_cb)
        data->received_cb(max_msg_id);
}

} // End of anonymous namespace

namespace {

// Returns true if the user is away from the notifications point of view: he is Away and
// mark_as_read_online_only option is enabled (the default).
bool is_away(PurpleConnection* gc)
{
    if (get_data(gc).options().mark_as_read_online_only) {
        PurpleStatus* status = purple_account_get_active_status(purple_connection_get_account(gc));
        PurpleStatusPrimitive primitive_status = purple_status_type_get_primitive(purple_status_get_type(status));
        if (primitive_status != PURPLE_STATUS_AVAILABLE)
            return true;
    }
    return false;
}

// Returns active PurpleConnection or nullptr.
PurpleConversation* find_active_conv(PurpleConnection* gc)
{
    for (GList* p = purple_get_conversations(); p; p = p->next) {
        PurpleConversation* conv = (PurpleConversation*)p->data;
        if (purple_conversation_get_gc(conv) == gc && purple_conversation_has_focus(conv))
            return conv;
    }
    return nullptr;
}

// Finds active user id or chat id. Both may be zero (if some other conversation is active).
void find_active_ids(PurpleConversation* conv, uint64* user_id, uint64* chat_id)
{
    if (!conv) {
        *user_id = 0;
        *chat_id = 0;
        return;
    }

    const char* name = purple_conversation_get_name(conv);
    *user_id = user_id_from_name(name, true);
    *chat_id = chat_id_from_name(name, true);

    if (user_id == 0 && chat_id == 0)
        vkcom_debug_info("Unknown conversation open: %s\n", name);
}

// Returns true if message belongs to the active conversation, which is defined by active_user_id
// and active_chat_id, returned from find_active_ids.
bool message_in_active(const VkReceivedMessage& msg, uint64 active_user_id, uint64 active_chat_id)
{
    if (active_chat_id != 0 && msg.chat_id == active_chat_id)
        return true;
    if (active_user_id != 0 && msg.user_id == active_user_id && msg.chat_id == 0)
        return true;
    return false;
}

template<typename Cont>
void mark_messages_as_read_impl(PurpleConnection* gc, const Cont& message_ids)
{
    if (message_ids.empty())
        return;

    vkcom_debug_info("Marking %d messages as read\n", (int)message_ids.size());
    vk_call_api_ids(gc, "messages.markAsRead", CallParams(), "message_ids", message_ids,
                    nullptr, nullptr, nullptr);
}

} // namespace

void mark_message_as_read(PurpleConnection* gc, const vector<VkReceivedMessage>& messages)
{
    VkData& gc_data = get_data(gc);

    // Check if we should defer all messages, because we are Away.
    if (is_away(gc)) {
        append(gc_data.deferred_mark_as_read, messages);
        return;
    }

    vector<uint64> message_ids;
    if (gc_data.options().mark_as_read_inactive_tab) {
        for (const VkReceivedMessage& msg: messages)
            message_ids.push_back(msg.msg_id);
    } else {
        PurpleConversation* conv = find_active_conv(gc);
        uint64 active_user_id;
        uint64 active_chat_id;
        find_active_ids(conv, &active_user_id, &active_chat_id);
        for (const VkReceivedMessage& msg: messages) {
            if (message_in_active(msg, active_user_id, active_chat_id))
                message_ids.push_back(msg.msg_id);
            else
                gc_data.deferred_mark_as_read.push_back(msg);
        }
    }

    mark_messages_as_read_impl(gc, message_ids);
}


void mark_deferred_messages_as_read(PurpleConnection* gc, bool active)
{
    if (is_away(gc) && !active)
        return;

    VkData& gc_data = get_data(gc);
    vector<uint64> message_ids;
    if (gc_data.options().mark_as_read_inactive_tab) {
        for (const VkReceivedMessage& msg: gc_data.deferred_mark_as_read)
            message_ids.push_back(msg.msg_id);
        gc_data.deferred_mark_as_read.clear();
    } else {
        PurpleConversation* conv = find_active_conv(gc);
        uint64 active_user_id;
        uint64 active_chat_id;
        find_active_ids(conv, &active_user_id, &active_chat_id);

        for (const VkReceivedMessage& msg: gc_data.deferred_mark_as_read)
            if (message_in_active(msg, active_user_id, active_chat_id))
                message_ids.push_back(msg.msg_id);

        erase_if(gc_data.deferred_mark_as_read, [=](const VkReceivedMessage& msg) {
            return message_in_active(msg, active_user_id, active_chat_id);
        });
    }

    mark_messages_as_read_impl(gc, message_ids);
}
