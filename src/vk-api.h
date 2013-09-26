// Vk.com API calling

#pragma once

#include "common.h"

#include <connection.h>

#include "contrib/picojson.h"

using CallParams = vector<string_pair>;
using CallSuccessCb = std::function<void(const picojson::value& result)>;
using CallErrorCb = std::function<void(const picojson::value& error)>;

void vk_call_api(PurpleConnection* gc, const char* method_name, const CallParams& params,
                 const CallSuccessCb& success_cb = nullptr, const CallErrorCb& error_cb = nullptr);
