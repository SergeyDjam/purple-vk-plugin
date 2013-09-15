#include <debug.h>
#include <util.h>

#include "vk-captcha.h"
#include "vk-common.h"
#include "vk-message.h"
#include "utils.h"

#include "vk-api.h"

namespace
{

// Helper struct used to reduce length of function signatures.
struct MessageData
{
    string uid;
    string message;
    SendSuccessCb success_cb;
    ErrorCb error_cb;
};

// Helper function, used in send_im_message and request_captcha.
void send_im_message_internal(PurpleConnection* gc, const MessageData& message, const string& captcha_sid = "",
                              const string& captcha_key = "");

} // End of anonymous namespace

int send_im_message(PurpleConnection* gc, const char* uid, const char* message,
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
void process_im_error(const picojson::value& error, PurpleConnection* gc, const MessageData& message);

void send_im_message_internal(PurpleConnection* gc, const MessageData& message, const string& captcha_sid,
                              const string& captcha_key)
{
    CallParams params = { {"user_id", message.uid}, {"message", message.message}, {"type", "1"} };
    if (!captcha_sid.empty())
        params.push_back(make_pair("captcha_sid", captcha_sid));
    if (!captcha_key.empty())
        params.push_back(make_pair("captcha_key", captcha_key));
    vk_call_api(gc, "messages.send", params, [=](const picojson::value&) {
        if (message.success_cb)
            message.success_cb();
    }, [=](const picojson::value& error) {
        process_im_error(error, gc, message);
    });
}

// Add error message to debug log, message window and call error_cb
void show_error(PurpleConnection* gc, const string& uid, const MessageData& message);

PurpleConversation* find_conv_for_uid(PurpleConnection* gc, const string& uid)
{
    return purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, ("id" + uid).c_str(),
                                                 purple_connection_get_account(gc));
}

void process_im_error(const picojson::value& error, PurpleConnection* gc, const MessageData& message)
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

void show_error(PurpleConnection* gc, const string& uid, const MessageData& message)
{
    purple_debug_error("prpl-vkcom", "Error sending message to %s: %s\n", message.uid.c_str(), message.message.c_str());

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
