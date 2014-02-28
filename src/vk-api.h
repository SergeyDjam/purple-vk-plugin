// Vk.com API calling

#pragma once

#include "common.h"

#include <connection.h>

#include "contrib/picojson.h"

// Calls method with params.
typedef vector<string_pair> CallParams;
typedef std::function<void(const picojson::value& result)> CallSuccessCb;
typedef std::function<void(const picojson::value& error)> CallErrorCb;
void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb, const CallErrorCb& error_cb);

// Helper function for calling APIs with "messages.get" or "messages.getDialogs" which return "items" array
// as a part of return value and may accept "offset" as a parameter.
//
// pagination is true for methods which accept "offset", false otherwise,
// call_process_item_cb is called for each item in the array,
// call_finished_cb is called upon completion,
// error_cb is called upon error.
typedef std::function<void(const picojson::value&)> CallProcessItemCb;
typedef std::function<void()> CallFinishedCb;
void vk_call_api_items(PurpleConnection* gc, const char* method_name, const CallParams& params,
                       bool pagination, const CallProcessItemCb& call_process_item_cb,
                       const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb);

// Helper function, which calls method for multiple ids (with the same other parameters), separated by comma.
// It is used if ids.size() is large (potentiall > 200 elements), so that multiple calls are required in order
// to fit into the URL limits. success_cb may be called multiple times and either call_finished_cb or error_cb
// will be called.
void vk_call_api_ids(PurpleConnection* gc, const char* method_name, const CallParams& params,
                     const char* id_param_name, const uint64_vec& id_values, const CallSuccessCb& success_cb,
                     const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb);
