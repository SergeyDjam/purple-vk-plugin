#include <debug.h>
#include <request.h>

#include "contrib/purple/http.h"
#include "strutils.h"

#include "vk-common.h"
#include "httputils.h"
#include "miscutils.h"

#include "vk-api.h"

const char api_version[] = "5.14";

namespace
{

// We store call parameters, because we may need to repeat the call on error.
struct VkCall
{
    string method_name;
    CallParams params;
};

// Callback, which is called upon receiving response to API call.
void on_vk_call_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, const VkCall& call,
                   const CallSuccessCb& success_cb, const CallErrorCb& error_cb);

} // End of anonymous namespace

void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb, const CallErrorCb& error_cb)
{
    vkcom_debug_info("    API call %s\n", method_name);

    VkData& gc_data = get_data(gc);
    if (gc_data.is_closing()) {
        vkcom_debug_error("Programming error: API method %s called during logout\n", method_name);
        return;
    }

    VkCall call;
    call.method_name = method_name;
    call.params = params;

    string method_url = str_format("https://api.vk.com/method/%s?v=%s&access_token=%s", method_name,
                                   api_version, gc_data.access_token().data());
    if (!params.empty()) {
        method_url += "&";
        method_url += urlencode_form(params);

        // MAX_URLENCODED_STRING is usually the largest params, all other parts of URL should
        // definitely be less than 300 bytes long.
        if (method_url.size() > MAX_URLENCODED_STRING + 300) {
            vkcom_debug_error("Too large method params length: %d\n", (int)method_url.size());

            if (error_cb)
                error_cb(picojson::value());
            return;
        }
    }

    PurpleHttpRequest* req = purple_http_request_new(method_url.data());
    purple_http_request_set_method(req, "POST");
    http_request(gc, req, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        // Connection has been cancelled due to account being disconnected. Do not do any response
        // processing, as callbacks may initiate new HTTP requests.
        if (get_data(gc).is_closing())
            return;

        on_vk_call_cb(http_conn, response, call, success_cb, error_cb);
    });
    purple_http_request_unref(req);
}

namespace
{

// Someone started authentication, waits until the auth token is set and repeats the call.
void vk_call_after_auth(PurpleConnection* gc, const VkCall& call,
                        const CallSuccessCb& success_cb, const CallErrorCb& error_cb)
{
    // Try repeating in a second.
    const int WAIT_AUTH_TIMEOUT = 1000;
    vkcom_debug_info("Authentication already in progress, retrying in %d msec\n", WAIT_AUTH_TIMEOUT);

    // This loop stops in case of failed authentication, because all timeouts die.
    timeout_add(gc, WAIT_AUTH_TIMEOUT, [=] {
        if (get_data(gc).is_authenticating())
            vk_call_after_auth(gc, call, success_cb, error_cb);
        else
            vk_call_api(gc, call.method_name.data(), call.params, success_cb, error_cb);
        return false;
    });
}

// Process error: maybe do another call and/or re-authorize.
void process_error(PurpleHttpConnection* http_conn, const picojson::value& error, const VkCall &call,
                   const CallSuccessCb& success_cb, const CallErrorCb& error_cb)
{
    if (!error.is<picojson::object>()) {
        vkcom_debug_error("Unknown error response: %s\n", error.serialize().data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    if (!field_is_present<double>(error, "error_code")) {
        vkcom_debug_error("Unknown error response: %s\n", error.serialize().data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    int error_code = error.get("error_code").get<double>();
    vkcom_debug_info("Got error code %d\n", error_code);
    PurpleConnection* gc = purple_http_conn_get_purple_connection(http_conn);
    VkData& gc_data = get_data(gc);

    if (error_code == VK_AUTHORIZATION_FAILED) {
        // Check if another authentication process has already started
        if (gc_data.is_authenticating()) {
            vk_call_after_auth(gc, call, success_cb, error_cb);
        } else {
            vkcom_debug_info("Access token expired, doing a reauthorization\n");

            gc_data.clear_access_token();
            gc_data.authenticate([=] {
                vk_call_api(gc, call.method_name.data(), call.params, success_cb, error_cb);
            }, [=] {
                if (error_cb)
                    error_cb(picojson::value());
            });
        }
    } else if (error_code == VK_TOO_MANY_REQUESTS_PER_SECOND) {
        const int RETRY_TIMEOUT = 400; // 400msec is less than 3 requests per second (the current rate limit on Vk.com
        vkcom_debug_info("Call rate limit hit, retrying in %d msec\n", RETRY_TIMEOUT);

        timeout_add(gc, RETRY_TIMEOUT, [=] {
            vk_call_api(gc, call.method_name.data(), call.params, success_cb, error_cb);
            return false;
        });
    } else if (error_code == VK_FLOOD_CONTROL) {
        // Simply ignore the error.
    } else if (error_code == VK_VALIDATION_REQUIRED) {
        // As far as I could understand, once you complete validation, all future requests/login
        // attempts will work correctly, so there is no need to do anything apart from showing
        // the link to the use and asking them to re-login.
        string redirect_uri;
        if (field_is_present<string>(error, "redirect_uri"))
            redirect_uri = error.get("redirect_uri").get<string>();

        vkcom_debug_info("Validation required, redirect uri: %s\n", redirect_uri.data());
        gc_data.clear_access_token();

        string message_text;
        if (redirect_uri.empty())
            message_text = i18n("Please open https://vk.com in your browser and validate yourself");
        else
            message_text = str_format(i18n("Please open the following link in your browser:\n%s"),
                                      redirect_uri.data());
        purple_request_action(nullptr, i18n("Please validate yourself"),
                              i18n("Please validate yourself"), message_text.data(), 0, nullptr,
                              nullptr, nullptr, nullptr, 1, i18n("Ok"), nullptr);

        // NOTE: Seems like NETWORK_ERROR is the only error, which Pidgin considers "non-fatal"
        // and does not require you to re-enable the account.
        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Validate yourself"));
        if (error_cb)
            error_cb(error);
    } else if (error_code == VK_INTERNAL_SERVER_ERROR) {
        // I've seen cases when internal server error has been reported due to incorrect
        // access token, let's clear it and re-authenticate for good measure.
        gc_data.clear_access_token();

        purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Internal server error"));
        if (error_cb)
            error_cb(error);
    } else {
        // We do not process captcha requests on API level, but we do not consider them errors
        if (error_code != VK_CAPTCHA_NEEDED) {
            string error_string = error.serialize();
            // Vk.com returns access_token among other error fields, let's remove it from the logs.
            str_replace(error_string, get_data(gc).access_token(), "XXX-ACCESS-TOKEN-XXX");
            vkcom_debug_error("Vk.com call error: %s\n", error_string.data());
        }

        if (error_cb)
            error_cb(error);
    }
}

void on_vk_call_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, const VkCall &call,
                   const CallSuccessCb& success_cb, const CallErrorCb& error_cb)
{
    if (!purple_http_response_is_successful(response)) {
        vkcom_debug_error("Error while calling API: %s\n", purple_http_response_get_error(response));
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    const char* response_text = purple_http_response_get_data(response, nullptr);
    const char* response_text_copy = response_text; // Picojson updates iterators it received.
    picojson::value root;
    string error = picojson::parse(root, response_text, response_text + strlen(response_text));
    if (!error.empty()) {
        vkcom_debug_error("Error parsing %s: %s\n", response_text_copy, error.data());
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    // Process all errors, potentially re-executing the request.
    if (root.contains("error")) {
        process_error(http_conn, root.get("error"), call, success_cb, error_cb);
        return;
    }

    if (!root.contains("response")) {
        vkcom_debug_error("Root element is neither \"response\" nor \"error\"\n");
        if (error_cb)
            error_cb(picojson::value());
        return;
    }

    if (success_cb)
        success_cb(root.get("response"));
}


// We do not want to copy CallParams when storing in lambda, the easiest way is storing them in shared_ptr.
typedef shared_ptr<CallParams> CallParams_ptr;

// Adds or replaces existing parameter value in CallParams.
void add_or_replace_call_param(CallParams& params, const char* name, const char* value)
{
    for (pair<string, string>& pair: params) {
        if (pair.first == name) {
            pair.second = value;
            return;
        }
    }
    params.emplace_back(name, value);
}

void vk_call_api_items_impl(PurpleConnection* gc, const char* method_name,
                            const CallParams_ptr& params, bool pagination,
                            const CallProcessItemCb& call_process_item_cb,
                            const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb,
                            size_t offset)
{
    if (offset > 0) {
        vkcom_debug_info("    API call with offset %d\n", (int)offset);
        add_or_replace_call_param(*params, "offset", to_string(offset).data());
    }

    vk_call_api(gc, method_name, *params, [=] (const picojson::value& result) {
        if (!field_is_present<picojson::array>(result, "items")
                || !field_is_present<double>(result, "count")) {
            vkcom_debug_error("Strange response, no 'count' and/or 'items' are present: %s\n",
                               result.serialize().data());
            if (error_cb)
                error_cb(picojson::value());
            return;
        }

        const picojson::array& items = result.get("items").get<picojson::array>();
        for (const picojson::value& v: items)
            call_process_item_cb(v);

        uint64 count = result.get("count").get<double>();
        size_t next_offset = offset + items.size();
        // Either we've received all items or method does not have pagination.
        if (next_offset >= count || items.empty() || !pagination) {
            if (call_finished_cb)
                call_finished_cb();
        } else {
            vk_call_api_items_impl(gc, method_name, params, pagination, call_process_item_cb,
                                   call_finished_cb, error_cb, next_offset);
        }
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
typedef shared_ptr<vector<uint64>> IdValues_ptr;

void vk_call_api_ids_impl(PurpleConnection* gc, const char* method_name, const CallParams_ptr& params,
                          const char* id_param_name, const IdValues_ptr& id_values, const CallSuccessCb& success_cb,
                          const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb, size_t offset)
{
    size_t num = max_urlencoded_int(id_values->data() + offset, id_values->data() + id_values->size());
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
                     const char* id_param_name, const vector<uint64>& id_values, const CallSuccessCb& success_cb,
                     const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb)
{
    CallParams_ptr params_ptr{ new CallParams(params) };
    IdValues_ptr id_values_ptr{ new vector<uint64>(id_values) };

    vk_call_api_ids_impl(gc, method_name, params_ptr, id_param_name, id_values_ptr, success_cb,
                         call_finished_cb, error_cb, 0);
}
