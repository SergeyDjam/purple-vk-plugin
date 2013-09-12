#include "vk-api.h"

#include "vk-common.h"
#include "vk-message.h"

void on_send_im_cb(const picojson::value& result, SendSuccessCb success_cb, ErrorCb error_cb)
{
}

void send_im_message(PurpleConnection* gc, const string& uid, const string& message, SendSuccessCb success_cb,
                     ErrorCb error_cb)
{
    VkConnData* data = (VkConnData*)purple_connection_get_protocol_data(gc);

    string_map params = { {"uid", uid}, {"message", message} };
    vk_call_api(gc, "messages.send", params, data->access_token(), [=](const picojson::value& result) {
        on_send_im_cb(result, success_cb, error_cb);
    }, nullptr); // TODO
}
