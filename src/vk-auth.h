// Vk.com authentication.

#pragma once

#include <connection.h>

#include "common.h"

typedef std::function<void(const string& auth_token, const string& uid)> AuthSuccessCb;

// Starts authentication process with given user id, password, client id and scope. Either success_cb or
// error_cb is called upon finishing authorization.
// NOTE: you should really call VkConnData::authenticate instead of this function.
void vk_auth_user(PurpleConnection* gc, const string& email, const string& password, const string& client_id,
                  const string& scope, const AuthSuccessCb& success_cb, const ErrorCb& error_cb = nullptr);
