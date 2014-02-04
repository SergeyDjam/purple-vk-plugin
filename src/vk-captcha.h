// User CAPTCHA input request.

#pragma once

#include "common.h"

#include <conversation.h>

// Downloads captcha_img, asks user to input captcha text and calls either got_text_cb or error_cb.
typedef std::function<void(const string& captcha_text)> CaptchaInputCb;
void request_captcha(PurpleConnection* gc, const string& captcha_img, const CaptchaInputCb& captcha_input_cb,
                     const ErrorCb& error_cb);
