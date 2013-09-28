#include <debug.h>
#include <imgstore.h>
#include <server.h>
#include <util.h>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"
#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-utils.h"

#include "vk-message-send.h"


namespace
{

// Parses and removes <img id="X"> out of message and returns cleaned message (without <img> tags)
// and list of img ids.
pair<string, int_vec> remove_img_tags(const char* message);

// Helper struct used to reduce length of function signatures.
struct SendMessage
{
    uint64 uid;
    string message;
    string attachments;
    SendSuccessCb success_cb;
    ErrorCb error_cb;
};

// Helper function, used in send_im_message and request_captcha.
void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid = "",
                              const string& captcha_key = "");

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, uint64 uid, const SendMessage& message);

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, uint64 uid, const char* raw_message,
                    const SendSuccessCb& success_cb, const ErrorCb& error_cb)
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
//    upload_images(gc, img_ids, [=](const string& img_attachments) {
//        string attachments = parse_vkcom_attachments(message);
//        // Append attachments for in-body images to other attachments.
//        if (!img_attachments.empty()) {
//            if (!attachments.empty())
//                attachments += ',';
//            attachments += img_attachments;
//        }
//        send_im_message_internal(gc, { uid, message, attachments, success_cb, error_cb });
//    }, [=] {
//        show_error(gc, uid, { uid, message, {}, success_cb, error_cb });
//    });
    return 1;
}

void send_im_attachment(PurpleConnection* gc, uint64 uid, const string& attachment)
{
    send_im_message_internal(gc, { uid, "", attachment, nullptr, nullptr });
}

namespace
{

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
        purple_debug_error("prpl-vkcom", "Unable to replace <img> in message %s\n", message);
        return { string(message), img_ids };
    }
    string cleaned_message = cleaned;
    g_free(cleaned);

    return { cleaned_message, img_ids };
}

// Process error and call either success_cb or error_cb. The only error which is meaningfully
// processed is CAPTCHA request.
void process_im_error(const picojson::value& error, PurpleConnection* gc, const SendMessage& message);

void send_im_message_internal(PurpleConnection* gc, const SendMessage& message, const string& captcha_sid,
                              const string& captcha_key)
{
    CallParams params = { {"user_id", to_string(message.uid)}, {"message", message.message},
                          {"attachment", message.attachments }, {"type", "1"} };
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

PurpleConversation* find_conv_for_uid(PurpleConnection* gc, uint64 uid)
{
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, buddy_name_from_uid(uid).data(),
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
    purple_debug_info("prpl-vkcom", "Received CAPTCHA %s\n", captcha_img.data());

    request_captcha(gc, captcha_img, [=](const string& captcha_key) {
        send_im_message_internal(gc, message, captcha_sid, captcha_key);
    }, [=] {
        show_error(gc, message.uid, message);
    });
}

void show_error(PurpleConnection* gc, uint64 uid, const SendMessage& message)
{
    purple_debug_error("prpl-vkcom", "Error sending message to %llu: %s\n", (unsigned long long)message.uid,
                       message.message.data());

    PurpleConversation* conv = find_conv_for_uid(gc, uid);
    if (conv) {
        char* escaped_message = g_markup_escape_text(message.message.data(), -1);
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
    CallParams params = { {"user_id", to_string(uid)}, {"type", "typing"} };
    vk_call_api(gc, "messages.setActivity", params);

    // Resend typing notification in 5 seconds
    return 5;
}
