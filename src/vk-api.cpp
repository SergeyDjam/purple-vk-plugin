#include <debug.h>
#include <request.h>

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
    } else if (error_code == VK_VALIDATION_REQUIRED) {
        // As far as I could understand, once you complete validation, all future requests/login attempts will work
        // correctly, so there is no need to do anything apart from showing the link to the user and asking them
        // to re-login.
        string message_text;
        if (!field_is_present<string>(error, "redirect_uri"))
            message_text = "Please open https://vk.com in your browser and validate yourself";
        else
            message_text = str_format("Please open the following link in your browser:\n%s",
                                      error.get("redirect_uri").get<string>().data());
        purple_request_action(nullptr, "Please validate yourself", "Please validate yourself", message_text.data(),
                              0, nullptr, nullptr, nullptr, nullptr, 1, "OK", nullptr);
        if (error_cb)
            error_cb(error);
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

// We do not want to copy CallParams when storing in lambda, the easiest way is storing them in shared_ptr.
typedef shared_ptr<CallParams> CallParams_ptr;

// Adds or replaces existing parameter value in CallParams.
void add_or_replace_call_param(CallParams& params, const char* name, const char* value)
{
    for (string_pair& pair: params) {
        if (pair.first == name) {
            pair.second = value;
            return;
        }
    }
    params.emplace_back(name, value);
}

void vk_call_api_items_impl(PurpleConnection* gc, const char* method_name, const CallParams_ptr& params,
                            bool pagination, const CallProcessItemCb& call_process_item_cb,
                            const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb, uint offset)
{
    if (offset > 0)
        add_or_replace_call_param(*params, "offset", to_string(offset).data());

    vk_call_api(gc, method_name, *params, [=] (const picojson::value& result) {
        if (!field_is_present<picojson::array>(result, "items")
                || !field_is_present<double>(result, "count")) {
            purple_debug_error("prpl-vkcom", "Strange response, no 'count' and/or 'items' are present: %s\n",
                               result.serialize().data());
            if (error_cb)
                error_cb(picojson::value());
            return;
        }

        const picojson::array& items = result.get("items").get<picojson::array>();
        for (const picojson::value& v: items)
            call_process_item_cb(v);

        uint64 count = result.get("count").get<double>();
        uint next_offset = offset + items.size();
        // Either we've received all items or method does not have pagination.
        if (next_offset >= count || items.size() == 0 || !pagination)
            call_finished_cb();
        else
            vk_call_api_items_impl(gc, method_name, params, pagination, call_process_item_cb,
                                   call_finished_cb, error_cb, next_offset);
    }, error_cb);
}

} // End of anonymous namespace

void vk_call_api_items(PurpleConnection* gc, const char* method_name, const CallParams& params, bool pagination,
                       const CallProcessItemCb& call_process_item_cb, const CallFinishedCb& call_finished_cb,
                       const CallErrorCb& error_cb)
{
    CallParams_ptr params_ptr{ new CallParams(params) };
    vk_call_api_items_impl(gc, method_name, params_ptr, pagination, call_process_item_cb,
                           call_finished_cb, error_cb, 0);
}

namespace
{

// We do not want to copy id_values when storing in lambda, the easiest way is storing them in shared_ptr.
typedef shared_ptr<uint64_vec> IdValues_ptr;

void vk_call_api_ids_impl(PurpleConnection* gc, const char* method_name, const CallParams_ptr& params,
                          const char* id_param_name, const IdValues_ptr& id_values, const CallSuccessCb& success_cb,
                          const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb, size_t offset)
{
    size_t num = max_urlencoded_int(id_values->begin() + offset, id_values->end());
    string ids_str = str_concat_int(',', id_values->begin() + offset, id_values->begin() + offset + num);
    add_or_replace_call_param(*params, id_param_name, ids_str.data());

    vk_call_api(gc, method_name, *params, [=](const picojson::value& v) {
        if (success_cb)
            success_cb(v);

        size_t next_offset = offset + num;
        if (next_offset < id_values->size()) {
            vk_call_api_ids_impl(gc, method_name, params, id_param_name, id_values, success_cb, call_finished_cb,
                                 error_cb, next_offset);
        } else {
            if (call_finished_cb)
                call_finished_cb();
        }
    }, error_cb);
}

} // anonymous namespace


void vk_call_api_ids(PurpleConnection* gc, const char* method_name, const CallParams& params,
                     const char* id_param_name, const uint64_vec& id_values, const CallSuccessCb& success_cb,
                     const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb)
{
    CallParams_ptr params_ptr{ new CallParams(params) };
    IdValues_ptr id_values_ptr{ new uint64_vec(id_values) };

    vk_call_api_ids_impl(gc, method_name, params_ptr, id_param_name, id_values_ptr, success_cb,
                         call_finished_cb, error_cb, 0);
}
