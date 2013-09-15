#pragma once

#include "common.h"

#include <connection.h>

#include "contrib/picojson.h"

using CallSuccessCb = std::function<void(const picojson::value& result)>;
using CallErrorCb = std::function<void(const picojson::value& error)>;

void vk_call_api(PurpleConnection* gc, const char* method_name, const string_map& params,
                 const CallSuccessCb& success_cb, const CallErrorCb& error_cb = nullptr);
