// User CAPTCHA input request.

#pragma once

#include "common.h"

#include <conversation.h>

// Downloads captcha_img, asks user to input captcha text and calls either got_text_cb
using CaptchaInputCb = std::function<void(const string& captcha_text)>;
void request_captcha(PurpleConnection* gc, const string& captcha_img, const CaptchaInputCb& captcha_input_cb,
                     const ErrorCb& error_cb);
