#include <debug.h>

#include "contrib/purple/http.h"

#include "vk-common.h"
#include "httputils.h"
#include "miscutils.h"

#include "vk-api.h"

namespace
{

// Callback, which is called upon receiving response to API call.
void on_vk_call_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, const CallSuccessCb& success_cb,
                   const CallErrorCb& error_cb);

} // End of anonymous namespace

void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb, const CallErrorCb& error_cb)
{
    VkConnData* conn_data = get_conn_data(gc);
    if (conn_data->is_closing()) {
        purple_debug_error("prpl-vkcom", "Programming error: API method %s called during logout\n", method_name);
        return;
    }

    string params_str = urlencode_form(params);
    string method_url = str_format("https://api.vk.com/method/%s?v=5.0&access_token=%s", method_name,
                                   conn_data->access_token().data());
    if (!params_str.empty()) {
        method_url += "&";
        method_url += params_str;
    }
    PurpleHttpRequest* req = purple_http_request_new(method_url.data());
    purple_http_request_set_method(req, "POST");
    http_request(gc, req, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        // Connection has been cancelled due to account being disconnected. Do not do any response
        // processing, as callbacks may initiate new HTTP requests.
        if (conn_data->is_closing())
            return;

        on_vk_call_cb(http_conn, response, success_cb, error_cb);
    });
    purple_http_request_unref(req);
}

namespace
{

// Process error: maybe do another call and/or re-authorize.
void process_error(PurpleHttpConnection* http_conn, const picojson::value& error, const CallSuccessCb& success_cb,
                   const CallErrorCb& error_cb);
// Repeats API call
void repeat_vk_call(PurpleConnection* gc, PurpleHttpRequest* req, const CallSuccessCb& success_cb,
                    const CallErrorCb& error_cb);

void on_vk_call_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, const CallSuccessCb& success_cb,
                   const CallErrorCb& error_cb)
{
    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error while calling API: %s\n", purple_http_response_get_error(response));
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    const char* response_text = purple_http_response_get_data(response, nullptr);
    const char* response_text_copy = response_text; // Picojson updates iterators it received.
    picojson::value root;
    string error = picojson::parse(root, response_text, response_text + strlen(response_text));
    if (!error.empty()) {
        purple_debug_error("prpl-vkcom", "Error parsing %s: %s\n", response_text_copy, error.data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    // Process all errors, potentially re-executing the request.
    if (root.contains("error")) {
        process_error(http_conn, root.get("error"), success_cb, error_cb);
        return;
    }

    if (!root.contains("response")) {
        purple_debug_error("prpl-vkcom", "Root element is neither \"response\" nor \"error\"\n");
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    if (success_cb)
        success_cb(root.get("response"));
}

void process_error(PurpleHttpConnection* http_conn, const picojson::value& error, const CallSuccessCb& success_cb,
                   const CallErrorCb& error_cb)
{
    if (!error.is<picojson::object>()) {
        purple_debug_error("prpl-vkcom", "Unknown error response: %s\n", error.serialize().data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    if (!field_is_present<double>(error, "error_code")) {
        purple_debug_error("prpl-vkcom", "Unknown error response: %s\n", error.serialize().data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    int error_code = error.get("error_code").get<double>();
    if (error_code == VK_AUTHORIZATION_FAILED || error_code == VK_TOO_MANY_REQUESTS_PER_SECOND) {
        PurpleConnection* gc = purple_http_conn_get_purple_connection(http_conn);
        PurpleHttpRequest* req = purple_http_conn_get_request(http_conn);
        purple_http_request_ref(req); // Increment references, or the request will die with http_conn.

        if (error_code == VK_AUTHORIZATION_FAILED) {
            purple_debug_info("prpl-vkcom", "Access token expired, doing a reauthorization\n");

            VkConnData* data = get_conn_data(gc);
            data->authenticate([=] {
                repeat_vk_call(gc, req, success_cb, error_cb);
            }, [=] {
                purple_http_request_unref(req);
                if (error_cb)
                    error_cb(picojson::value());
            });
            return;
        } else if (error_code == VK_TOO_MANY_REQUESTS_PER_SECOND) {
            const int RETRY_TIMEOUT = 400; // 400msec is less than 3 requests per second (the current rate limit on Vk.com
            purple_debug_info("prpl-vkcom", "Call rate limit hit, retrying in %d msec\n", RETRY_TIMEOUT);

            timeout_add(gc, RETRY_TIMEOUT, [=] {
                repeat_vk_call(gc, req, success_cb, error_cb);
                return false;
            });
        }
        return;
    } else if (error_code == VK_FLOOD_CONTROL) {
        return; // Simply ignore the error.
    }

    // We do not process captcha requests on API level, but we do not consider them errors
    if (error_code != VK_CAPTCHA_NEEDED) {
        string error_string = error.serialize();
        // Vk.com returns access_token among other error fields, let's remove it from the logs.
        VkConnData* conn_data = get_conn_data(purple_http_conn_get_purple_connection(http_conn));
        str_replace(error_string, conn_data->access_token(), "XXX-ACCESS-TOKEN-XXX");
        purple_debug_error("prpl-vkcom", "Vk.com call error: %s\n", error_string.data());
    }
    if (error_cb)
        error_cb(error);
}

void repeat_vk_call(PurpleConnection* gc, PurpleHttpRequest* req, const CallSuccessCb& success_cb,
                    const CallErrorCb& error_cb)
{
    http_request(gc, req, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        on_vk_call_cb(http_conn, response, success_cb, error_cb);
    });
    purple_http_request_unref(req);
}

} // End anonymous namespace


namespace
{

// Helper function for vk_call_api_items.
void vk_call_api_items_internal(PurpleConnection* gc, const char* method_name, const CallParams& params,
                                int offset, bool pagination, const CallProcessItemCb& call_process_item_cb,
                                const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb);

} // End of anonymous namespace

void vk_call_api_items(PurpleConnection* gc, const char* method_name, const CallParams& params, bool pagination,
                       const CallProcessItemCb& call_process_item_cb, const CallFinishedCb& call_finished_cb,
                       const CallErrorCb& error_cb)
{
    vk_call_api_items_internal(gc, method_name, params, 0, pagination, call_process_item_cb, call_finished_cb,
                               error_cb);
}

namespace
{

void vk_call_api_items_internal(PurpleConnection* gc, const char* method_name, const CallParams& params,
                                int offset, bool pagination, const CallProcessItemCb& call_process_item_cb,
                                const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb)
{
    auto process_items_cb = [=] (const picojson::value& result) {
        if (!field_is_present<picojson::array>(result, "items")) {
            purple_debug_error("prpl-vkcom", "Strange response, no 'count' and/or 'items' are present: %s\n",
                               result.serialize().data());
            if (error_cb)
                error_cb(picojson::value());
            return;
        }

        const picojson::array& items = result.get("items").get<picojson::array>();
        for (const picojson::value& v: items)
            call_process_item_cb(v);

        // Either we've received all items or method does not have pagination.
        if (!pagination || items.size() == 0)
            call_finished_cb();
        else
            vk_call_api_items_internal(gc, method_name, params, offset + items.size(), false,
                                       call_process_item_cb, call_finished_cb, error_cb);
    };

    if (!pagination || offset == 0) {
        vk_call_api(gc, method_name, params, process_items_cb, error_cb);
    } else {
        CallParams new_params = params;
        new_params.emplace_back("offset", to_string(offset));

        vk_call_api(gc, method_name, new_params, process_items_cb, error_cb);
    }
}

} // End of anonymous namespace
