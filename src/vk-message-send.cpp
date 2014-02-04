#include <debug.h>
#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-upload.h"
#include "vk-utils.h"

#include "vk-message-send.h"


namespace
{

// Common function for send_im_message and send_chat_message.
int send_message(PurpleConnection* gc, uint64 uid, uint64 chat_id, const char* raw_message,
                 const SuccessCb& success_cb, const ErrorCb& error_cb);

// Parses and removes <img id="X"> out of message and returns cleaned message (without <img> tags)
// and list of img ids.
pair<string, int_vec> remove_img_tags(const char* message);
// Uploads a number of images, stored in imgstore and returns the list of attachments to be added
// to the message which contained the images.
typedef std::function<void(const string& attachments)> ImagesUploadedCb;
void upload_imgstore_images(PurpleConnection* gc, const int_vec& img_ids, const ImagesUploadedCb& uploaded_cb,
                            const ErrorCb& error_cb);

// Helper struct used to reduce length of function signatures.
struct SendMessage
{
    // One and only one of uid or chat_id should be non-zero.
    uint64 uid;
    uint64 chat_id;
    string text;
    string attachments;
    SuccessCb success_cb;
    ErrorCb error_cb;
};

// Helper function, used in send_im_message and request_captcha.
void send_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid = "",
                              const string& captcha_key = "");

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, const SendMessage& message);

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, uint64 uid, const char* raw_message,
                    const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    if (uid == 0)
        return 0;
    return send_message(gc, uid, 0, raw_message, success_cb, error_cb);
}

int send_chat_message(PurpleConnection* gc, uint64 chat_id, const char* raw_message,
                      const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    if (chat_id == 0)
        return 0;
    return send_message(gc, 0, chat_id, raw_message, success_cb, error_cb);
}

void send_im_attachment(PurpleConnection* gc, uint64 uid, const string& attachment)
{
    send_message_internal(gc, { uid, 0, "", attachment, nullptr, nullptr });
}

namespace
{

int send_message(PurpleConnection* gc, uint64 uid, uint64 chat_id, const char* raw_message,
                 const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    // NOTE: We de-escape message before sending, because
    //  * Vk.com chat is plaintext anyway
    //  * Vk.com accepts '\n' in place of <br>
    //  * we do not receive HTML (apart from Pidgin insistence to send HTML entities)
    char* unescaped_message = purple_unescape_html(raw_message);
    // We remove all <img id="X">, inserted via "Insert image", upload the images to server
    // and append to the attachment.
    pair<string, int_vec> p = remove_img_tags(unescaped_message);
    g_free(unescaped_message);

    const string& message = p.first;
    const int_vec& img_ids = p.second;
    upload_imgstore_images(gc, img_ids, [=](const string& img_attachments) {
        string attachments = parse_vkcom_attachments(message);
        // Append attachments for in-body images to other attachments.
        if (!img_attachments.empty()) {
            if (!attachments.empty())
                attachments += ',';
            attachments += img_attachments;
        }
        send_message_internal(gc, { uid, chat_id, message, attachments, success_cb, error_cb });
    }, [=] {
        show_error(gc, { uid, chat_id, message, {}, success_cb, error_cb });
    });

    add_buddy_if_needed(gc, uid);

    return 1;

}

pair<string, int_vec> remove_img_tags(const char* message)
{
    static GRegex* img_regex = nullptr;
    static OnExit img_regex_deleter([=] {
        g_regex_unref(img_regex);
    });

    if (!img_regex) {
        const char img_regex_const[] = "<img id=\"(?<id>\\d+)\">";
        img_regex = g_regex_new(img_regex_const, GRegexCompileFlags(G_REGEX_CASELESS | G_REGEX_OPTIMIZE),
                                  GRegexMatchFlags(0), nullptr);
        if (!img_regex) {
            purple_debug_error("prpl-vkcom", "Unable to compile <img> regexp, aborting");
            return { string(message), {} };
        }
    }

    GMatchInfo* match_info;
    if (!g_regex_match(img_regex, message, GRegexMatchFlags(0), &match_info)) {
        return { string(message), {} };
    }

    // Find all img ids.
    int_vec img_ids;
    while (g_match_info_matches(match_info)) {
        char* id = g_match_info_fetch_named(match_info, "id");
        img_ids.push_back(atoi(id));

        g_match_info_next(match_info, nullptr);
    }

    // Clean message.
    char* cleaned = g_regex_replace_literal(img_regex, message, -1, 0, "", GRegexMatchFlags(0), nullptr);
    if (!cleaned) {
        purple_debug_error("prpl-vkcom", "Unable to replace <img> in message\n");
        return { string(message), img_ids };
    }
    string cleaned_message = cleaned;
    g_free(cleaned);

    return { cleaned_message, img_ids };
}

// Helper data structure for upload_imgstore_images.
struct UploadImgstoreImages
{
    // List of all img_ids
    int_vec img_ids;
    // attachments from all the uploaded img_ids.
    string attachments;

    ImagesUploadedCb uploaded_cb;
    ErrorCb error_cb;
};
typedef shared_ptr<UploadImgstoreImages> UploadImgstoreImagesPtr;

// Helper function for upload_imgstore_images.
void upload_imgstore_images_internal(PurpleConnection* gc, const UploadImgstoreImagesPtr& data);

void upload_imgstore_images(PurpleConnection* gc, const int_vec& img_ids, const ImagesUploadedCb& uploaded_cb,
                            const ErrorCb& error_cb)
{
    if (img_ids.empty()) {
        uploaded_cb("");
        return;
    }

    // GCC 4.6 crashes here if we try to use uniform intialization.
    UploadImgstoreImagesPtr data{ new UploadImgstoreImages() };
    data->img_ids = img_ids;
    data->uploaded_cb = uploaded_cb;
    data->error_cb  = error_cb;
    // Reverse data->img_ids as we start pop the items from the back of the img_ids.
    std::reverse(data->img_ids.begin(), data->img_ids.end());
    upload_imgstore_images_internal(gc, data);
}

void upload_imgstore_images_internal(PurpleConnection* gc, const UploadImgstoreImagesPtr& data)
{
    int img_id = data->img_ids.back();
    PurpleStoredImage* img = purple_imgstore_find_by_id(img_id);
    const char* filename = purple_imgstore_get_filename(img);
    const void* contents = purple_imgstore_get_data(img);
    size_t size = purple_imgstore_get_size(img);

    purple_debug_info("prpl-vkcom", "Uploading img %d\n", img_id);
    upload_photo_for_im(gc, filename, contents, size, [=](const picojson::value& v) {
        purple_debug_info("prpl-vkcom", "Sucessfully uploaded img %d\n", img_id);
        if (!v.is<picojson::array>() || !v.contains(0)) {
            purple_debug_error("prpl-vkcom", "Unknown photos.saveMessagesPhoto result: %s\n", v.serialize().data());
            if (data->error_cb)
                data->error_cb();
            return;
        }
        const picojson::value& fields = v.get(0);
        if (!field_is_present<double>(fields, "owner_id") || !field_is_present<double>(fields, "id")) {
            purple_debug_error("prpl-vkcom", "Unknown photos.saveMessagesPhoto result: %s\n", v.serialize().data());
            if (data->error_cb)
                data->error_cb();
            return;
        }

        if (!data->attachments.empty())
            data->attachments += ',';
        // NOTE: We do not receive "access_key" from photos.saveMessagesPhoto, but it seems it does not matter,
        // vk.com will automatically add access_key to your private photos.
        int64 owner_id = int64(fields.get("owner_id").get<double>());
        uint64 id = uint64(fields.get("id").get<double>());
        data->attachments += str_format("photo%lld_%lld", (long long)owner_id, (long long)id);

        data->img_ids.pop_back();
        if (data->img_ids.empty()) {
            data->uploaded_cb(data->attachments);
            return;
        } else {
            upload_imgstore_images_internal(gc, data);
        }
    }, [=] {
        if (data->error_cb)
            data->error_cb();
    });

}

// Process error and call either success_cb or error_cb. The only error which is meaningfully
// processed is CAPTCHA request.
void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message);

// We cannot send large messages at once due to URL limits (message is encoded in URL). The limit of 1200
// characters is rather arbitrary, testing shows that it is usually ok.
const uint MESSAGE_TEXT_LIMIT = 1200;

void send_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid,
                           const string& captcha_key)
{
    string text = message.text.substr(0, MESSAGE_TEXT_LIMIT);
    CallParams params = { {"message", text}, {"attachment", message.attachments }, {"type", "1"} };
    if (message.uid > 0)
        params.emplace_back("user_id", to_string(message.uid));
    else
        params.emplace_back("chat_id", to_string(message.chat_id));
    if (!captcha_sid.empty())
        params.emplace_back("captcha_sid", captcha_sid);
    if (!captcha_key.empty())
        params.emplace_back("captcha_key", captcha_key);

    VkConnData* conn_data = get_conn_data(gc);
    steady_time_point current_time = steady_clock::now();
    assert(conn_data->last_msg_sent_time <= current_time);
    conn_data->last_msg_sent_time = current_time;

    // Next part of message, if message is long enough.
    SendMessage next_message;
    if (message.text.length() > MESSAGE_TEXT_LIMIT) {
        next_message.uid = message.uid;
        next_message.chat_id = message.chat_id;
        next_message.success_cb = message.success_cb;
        next_message.error_cb = message.error_cb;
        next_message.text = message.text.substr(MESSAGE_TEXT_LIMIT);
    }

    vk_call_api(gc, "messages.send", params, [=](const picojson::value& v) {
        if (!v.is<double>()) {
            purple_debug_error("prpl-vkcom", "Wrong response from message.send: %s\n", v.serialize().data());
            if (message.error_cb)
                message.error_cb();
            return;
        }

        // NOTE: We do not set last_msg_id here, because it is done when corresponding notification is received
        // in longpoll.
        uint64 msg_id = v.get<double>();
        conn_data->sent_msg_ids.insert(msg_id);

        // We have sent the whole message.
        if (next_message.text.empty()) {
            if (message.success_cb)
                message.success_cb();
        } else {
            send_message_internal(gc, next_message, captcha_sid, captcha_key);
        }
    }, [=](const picojson::value& error) {
        process_im_error(error, gc, message);
    });
}

void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message)
{
    if (!error.is<picojson::object>() || !field_is_present<double>(error, "error_code")) {
        // Most probably, network timeout.
        show_error(gc, message);
        return;
    }
    int error_code = error.get("error_code").get<double>();
    if (error_code != VK_CAPTCHA_NEEDED) {
        show_error(gc, message);
        return;
    }
    if (!field_is_present<string>(error, "captcha_sid") || !field_is_present<string>(error, "captcha_img")) {
        purple_debug_error("prpl-vkcom", "Captcha request does not contain captcha_sid or captcha_img");
        show_error(gc, message);
        return;
    }

    const string& captcha_sid = error.get("captcha_sid").get<string>();
    const string& captcha_img = error.get("captcha_img").get<string>();
    purple_debug_info("prpl-vkcom", "Received CAPTCHA %s\n", captcha_img.data());

    request_captcha(gc, captcha_img, [=](const string& captcha_key) {
        send_message_internal(gc, message, captcha_sid, captcha_key);
    }, [=] {
        show_error(gc, message);
    });
}

void show_error(PurpleConnection* gc, const SendMessage& message)
{
    purple_debug_error("prpl-vkcom", "Error sending message to %llu/%llu\n",
                       (unsigned long long)message.uid, (unsigned long long)message.chat_id);

    PurpleConversation* conv = find_conv_for_id(gc, message.uid, message.chat_id);
    if (conv) {
        char* escaped_message = g_markup_escape_text(message.text.data(), -1);
        string error_msg = str_format("Error sending message '%s'", escaped_message);
        purple_conversation_write(conv, nullptr, error_msg.data(),
                                  PurpleMessageFlags(PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NO_LINKIFY), time(nullptr));
        g_free(escaped_message);
    }

    if (message.error_cb)
        message.error_cb();
}

} // End of anonymous namespace


unsigned send_typing_notification(PurpleConnection* gc, uint64 uid)
{
    if (uid == 0)
        return 0;

    CallParams params = { {"user_id", to_string(uid)}, {"type", "typing"} };
    vk_call_api(gc, "messages.setActivity", params);

    add_buddy_if_needed(gc, uid);

    // Resend typing notification in 5 seconds
    return 5;
}
