// Vk.com API calling

#pragma once

#include <utility>

using std::pair;

#include "common.h"

#include <connection.h>

#include "contrib/picojson/picojson.h"

// Calls method with params.
typedef vector<pair<string, string>> CallParams;
typedef function_ptr<void(const picojson::value& result)> CallSuccessCb;
typedef function_ptr<void(const picojson::value& error)> CallErrorCb;
void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb, const CallErrorCb& error_cb);

// Helper function for calling APIs with "messages.get" or "messages.getDialogs" which return
// "items" array as a part of return value and may accept "offset" as a parameter.
//
// pagination is true for methods which accept "offset", false otherwise,
// call_process_item_cb is called for each item in the array,
// call_finished_cb is called upon completion,
// error_cb is called upon error.
typedef function_ptr<void(const picojson::value&)> CallProcessItemCb;
typedef function_ptr<void()> CallFinishedCb;
void vk_call_api_items(PurpleConnection* gc, const char* method_name, const CallParams& params,
                       bool pagination, const CallProcessItemCb& call_process_item_cb,
                       const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb);
