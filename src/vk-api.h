// Vk.com API calling

#pragma once

#include "common.h"

#include <connection.h>

#include "contrib/picojson.h"

// Calls method with params.
using CallParams = vector<string_pair>;
using CallSuccessCb = std::function<void(const picojson::value& result)>;
using CallErrorCb = std::function<void(const picojson::value& error)>;
void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb = nullptr, const CallErrorCb& error_cb = nullptr);

// Helper function for calling APIs with "messages.get" or "messages.getDialogs" which return "items" array
// as a part of return value and may accept "offset" as a parameter.
//
// pagination is true for methods which accept "offset", false otherwise,
// call_process_item_cb is called for each item in the array,
// call_finished_cb is called upon completion,
// error_cb is called upon error.
using CallProcessItemCb = std::function<void(const picojson::value&)>;
using CallFinishedCb = std::function<void()>;
void vk_call_api_items(PurpleConnection* gc, const char* method_name, const CallParams& params,
                       bool pagination, const CallProcessItemCb& call_process_item_cb,
                       const CallFinishedCb& call_finished_cb, const CallErrorCb& error_cb = nullptr);
