#include <debug.h>
#include <request.h>

#include "httputils.h"

#include "vk-captcha.h"

namespace
{

// user_data structure for request_captcha_ok and request_captcha_cancel
struct CaptchaRequestData
{
    CaptchaInputCb captcha_input_cb;
    ErrorCb error_cb;
    // The next two fields are used to repeat captcha input if user did not enter any.
    PurpleConnection* gc;
    string captcha_img;
};

// Calls captcha_input_cb after user entered captcha text.
void request_captcha_ok(CaptchaRequestData* data, PurpleRequestFields* fields)
{
    const char* captcha_key = purple_request_fields_get_string(fields, "captcha_text");
    if (!captcha_key || captcha_key[0] == '\0') {
        // User did not enter anything, most likely accidentally pressed Enter, let's redo captcha.
        request_captcha(data->gc, data->captcha_img, data->captcha_input_cb, data->error_cb);
        delete data;
        return;
    }

    vkcom_debug_info("Captcha entered\n");

    data->captcha_input_cb(captcha_key);
    delete data;
}

// Shows error after user cancelled captcha request
void request_captcha_cancel(CaptchaRequestData* data, PurpleRequestFields*)
{
    vkcom_debug_info("Captcha entry cancelled by user\n");
    if (data->error_cb)
        data->error_cb();
    delete data;
}

} // End of anonymous namespace

void request_captcha(PurpleConnection* gc, const string& captcha_img, const CaptchaInputCb& captcha_input_cb, const ErrorCb& error_cb)
{
    http_get(gc, captcha_img, [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (!purple_http_response_is_successful(response)) {
            vkcom_debug_error("Error while fetching captcha: %s\n",
                               purple_http_response_get_error(response));
            if (error_cb)
                error_cb();
            return;
        }

        vkcom_debug_info("Got captcha %s, showing to user\n", captcha_img.data());

        size_t captcha_len;
        const char* captcha_data = purple_http_response_get_data(response, &captcha_len);

        PurpleRequestFields* fields = purple_request_fields_new();
        PurpleRequestFieldGroup* field_group = purple_request_field_group_new(nullptr);
        purple_request_fields_add_group(fields, field_group);
        PurpleRequestField* field;
        field = purple_request_field_image_new("captcha_img", i18n("Captcha"), captcha_data,
                                               captcha_len);
        purple_request_field_group_add_field(field_group, field);
        field = purple_request_field_string_new("captcha_text", i18n("Text"), "", false);
        purple_request_field_string_set_masked(field, false);
        purple_request_field_group_add_field(field_group, field);

        CaptchaRequestData* data = new CaptchaRequestData({ captcha_input_cb, error_cb, gc,
                                                            captcha_img });
        purple_request_fields(gc, i18n("Are you classified as human?"),
                              i18n("Are you classified as human?"), nullptr, fields,
                              i18n("Ok"), G_CALLBACK(request_captcha_ok),
                              i18n("Cancel"), G_CALLBACK(request_captcha_cancel),
                              purple_connection_get_account(gc), nullptr, nullptr, data);
    });
}
