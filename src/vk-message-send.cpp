#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "strutils.h"

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-buddy.h"
#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-smileys.h"
#include "vk-upload.h"
#include "vk-utils.h"

#include "vk-message-send.h"


namespace
{

// Common function for send_im_message and send_chat_message.
int send_message(PurpleConnection* gc, uint64 user_id, uint64 chat_id, const char* raw_message,
                 const SuccessCb& success_cb, const ErrorCb& error_cb);

// Helper struct used to reduce length of function signatures.
struct SendMessage
{
    // One and only one of user_id or chat_id should be non-zero.
    uint64 user_id;
    uint64 chat_id;
    string text;
    string attachments;
    SuccessCb success_cb;
    ErrorCb error_cb;
};
typedef shared_ptr<SendMessage> SendMessage_ptr;

// Helper function, used in send_im_message and request_captcha.
void send_message_internal(PurpleConnection* gc, const SendMessage_ptr& message,
                           const string& captcha_sid = "", const string& captcha_key = "");

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, const SendMessage& message);

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, uint64 user_id, const char* raw_message,
                    const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    vkcom_debug_info("Sending IM message to %llu\n", (unsigned long long)user_id);

    return send_message(gc, user_id, 0, raw_message, success_cb, error_cb);
}

int send_chat_message(PurpleConnection* gc, uint64 chat_id, const char* raw_message,
                      const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    vkcom_debug_info("Sending chat message to %llu\n", (unsigned long long)chat_id);

    return send_message(gc, 0, chat_id, raw_message, success_cb, error_cb);
}

void send_im_attachment(PurpleConnection* gc, uint64 user_id, const string& attachment)
{
    SendMessage_ptr message{ new SendMessage() };
    message->user_id = user_id;
    message->chat_id = 0;
    message->attachments = attachment;

    vkcom_debug_info("Sending IM attachment\n");

    send_message_internal(gc, message);
}

namespace
{

// Parses and removes <img id="X"> out of message and returns cleaned message (without <img> tags)
// and list of img ids.
void remove_img_tags(const char* message, string* clean_message, vector<int>* img_ids)
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
            vkcom_debug_error("Unable to compile <img> regexp, aborting");
            return;
        }
    }

    GMatchInfo* match_info;
    if (!g_regex_match(img_regex, message, GRegexMatchFlags(0), &match_info)) {
        *clean_message = message;
        return;
    }

    // Find all img ids.
    while (g_match_info_matches(match_info)) {
        char* id = g_match_info_fetch_named(match_info, "id");
        img_ids->push_back(atoi(id));

        g_match_info_next(match_info, nullptr);
    }

    // Clean message.
    char* cleaned = g_regex_replace_literal(img_regex, message, -1, 0, "", GRegexMatchFlags(0), nullptr);
    if (!cleaned) {
        vkcom_debug_error("Unable to replace <img> in message\n");

        *clean_message = message;
        return;
    }

    *clean_message = cleaned;
    g_free(cleaned);
}

// Helper data structure for upload_imgstore_images.
struct UploadImgstoreImages
{
    // List remaining all img_ids to upload.
    vector<int> img_ids;
    // Attachments created from the already uploaded img_ids.
    string attachments;
};
typedef shared_ptr<UploadImgstoreImages> UploadImgstoreImages_ptr;

typedef function_ptr<void(const string& attachments)> ImagesUploadedCb;

void upload_imgstore_images_impl(PurpleConnection* gc, const UploadImgstoreImages_ptr& images,
                                 const ImagesUploadedCb& uploaded_cb, const ErrorCb& error_cb, int offset)
{
    // We start uploading images from the end.
    int img_id = images->img_ids[images->img_ids.size() - offset - 1];
    PurpleStoredImage* img = purple_imgstore_find_by_id(img_id);
    const char* filename = purple_imgstore_get_filename(img);
    const void* contents = purple_imgstore_get_data(img);
    size_t size = purple_imgstore_get_size(img);

    vkcom_debug_info("Uploading img %d\n", img_id);
    upload_photo_for_im(gc, filename, contents, size, [=](const picojson::value& v) {
        vkcom_debug_info("Sucessfully uploaded img %d\n", img_id);
        if (!v.is<picojson::array>() || !v.contains(0)) {
            vkcom_debug_error("Unknown photos.saveMessagesPhoto result: %s\n", v.serialize().data());
            if (error_cb)
                error_cb();
            return;
        }
        const picojson::value& fields = v.get(0);
        if (!field_is_present<double>(fields, "owner_id") || !field_is_present<double>(fields, "id")) {
            vkcom_debug_error("Unknown photos.saveMessagesPhoto result: %s\n", v.serialize().data());
            if (error_cb)
                error_cb();
            return;
        }

        if (!images->attachments.empty())
            images->attachments += ',';
        // NOTE: We do not receive "access_key" from photos.saveMessagesPhoto, but it seems it does not matter,
        // vk.com will automatically add access_key to your private photos.
        int64 owner_id = (int64)fields.get("owner_id").get<double>();
        uint64 id = (uint64)fields.get("id").get<double>();
        images->attachments += str_format("photo%lld_%llu", (long long)owner_id,
                                          (unsigned long long)id);

        if ((size_t)offset == images->img_ids.size() - 1) {
            uploaded_cb(images->attachments);
        } else {
            upload_imgstore_images_impl(gc, images, uploaded_cb, error_cb, offset + 1);
        }
    }, [=] {
        if (error_cb)
            error_cb();
    });
}

// Uploads a number of images, stored in imgstore and returns the list of attachments to be added
// to the message which contained the images.
void upload_imgstore_images(PurpleConnection* gc, const vector<int>& img_ids, const ImagesUploadedCb& uploaded_cb,
                            const ErrorCb& error_cb)
{
    if (img_ids.empty()) {
        uploaded_cb("");
        return;
    }

    // GCC 4.6 crashes here if we try to use uniform intialization.
    UploadImgstoreImages_ptr images{ new UploadImgstoreImages() };
    images->img_ids = img_ids;
    upload_imgstore_images_impl(gc, images, uploaded_cb, error_cb, 0);
}

int send_message(PurpleConnection* gc, uint64 user_id, uint64 chat_id, const char* raw_message,
                 const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    // We remove all <img id="X">, inserted via "Insert image", upload the images to server
    // and append to the attachment.
    string no_imgs_message;
    vector<int> img_ids;
    remove_img_tags(raw_message, &no_imgs_message, &img_ids);

    // Strip HTML tags from the message (<a> gets replaced with link title + url, most other
    // tags simply removed).
    char* stripped_message = purple_markup_strip_html(no_imgs_message.data());
    SendMessage_ptr message{ new SendMessage() };
    message->user_id = user_id;
    message->chat_id = chat_id;
    message->text = stripped_message;
    message->success_cb = success_cb;
    message->error_cb = error_cb;
    g_free(stripped_message);

    convert_outgoing_smileys(message->text);

    upload_imgstore_images(gc, img_ids, [=](const string& img_attachments) {
        message->attachments = parse_vkcom_attachments(message->text);
        // Append attachments for in-body images to other attachments.
        if (!img_attachments.empty()) {
            if (!message->attachments.empty())
                message->attachments += ',';
            message->attachments += img_attachments;
        }

        send_message_internal(gc, message);
    }, [=] {
        show_error(gc, *message);
    });

    if (user_id != 0)
        add_buddy_if_needed(gc, user_id);

    return 1;
}

// Process error and call either success_cb or error_cb. The only error which is meaningfully
// processed is CAPTCHA request.
void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage_ptr& message);

void send_message_internal(PurpleConnection* gc, const SendMessage_ptr& message, const string& captcha_sid,
                           const string& captcha_key)
{
    CallParams params = { {"attachment", message->attachments }, {"type", "1"} };

    params.emplace_back("message", message->text);
    if (message->user_id != 0)
        params.emplace_back("user_id", to_string(message->user_id));
    else
        params.emplace_back("chat_id", to_string(message->chat_id));
    if (!captcha_sid.empty())
        params.emplace_back("captcha_sid", captcha_sid);
    if (!captcha_key.empty())
        params.emplace_back("captcha_key", captcha_key);

    get_data(gc).set_last_msg_sent_time(steady_clock::now());

    vk_call_api(gc, "messages.send", params, [=](const picojson::value& v) {
        if (!v.is<double>()) {
            vkcom_debug_error("Wrong response from message.send: %s\n", v.serialize().data());
            show_error(gc, *message);
            return;
        }

        // NOTE: We do not set last_msg_id here, because it is done when corresponding notification is received
        // in longpoll.
        uint64 msg_id = v.get<double>();
        get_data(gc).add_sent_msg_id(msg_id);

        if (message->success_cb)
            message->success_cb();
    }, [=](const picojson::value& error) {
        process_im_error(error, gc, message);
    });
}

void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage_ptr& message)
{
    if (!error.is<picojson::object>() || !field_is_present<double>(error, "error_code")) {
        // Most probably, network timeout.
        show_error(gc, *message);
        return;
    }
    int error_code = error.get("error_code").get<double>();
    if (error_code != VK_CAPTCHA_NEEDED) {
        show_error(gc, *message);
        return;
    }
    if (!field_is_present<string>(error, "captcha_sid") || !field_is_present<string>(error, "captcha_img")) {
        vkcom_debug_error("Captcha request does not contain captcha_sid or captcha_img");
        show_error(gc, *message);
        return;
    }

    const string& captcha_sid = error.get("captcha_sid").get<string>();
    const string& captcha_img = error.get("captcha_img").get<string>();

    vkcom_debug_info("Received catpcha %s\n", captcha_img.data());

    request_captcha(gc, captcha_img, [=](const string& captcha_key) {
        send_message_internal(gc, message, captcha_sid, captcha_key);
    }, [=] {
        show_error(gc, *message);
    });
}

void show_error(PurpleConnection* gc, const SendMessage& message)
{
    vkcom_debug_error("Error sending message to %llu/%llu\n", (unsigned long long)message.user_id,
                      (unsigned long long)message.chat_id);

    PurpleConversation* conv = find_conv_for_id(gc, message.user_id, message.chat_id);
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


unsigned send_typing_notification(PurpleConnection* gc, uint64 user_id)
{
    CallParams params = { {"user_id", to_string(user_id)}, {"type", "typing"} };
    vk_call_api(gc, "messages.setActivity", params, nullptr, nullptr);

    add_buddy_if_needed(gc, user_id);

    // Resend typing notification in 10 seconds
    return 10;
}
