#include <debug.h>

#include "miscutils.h"
#include "vk-api.h"
#include "vk-common.h"
#include "vk-message-send.h"
#include "vk-upload.h"
#include "vk-utils.h"

#include "vk-filexfer.h"

namespace
{

// Starts xfer. Finds out upload server URL, creates the full request for upload server and writes it.
// There seems to be no reason to call purple_xfer_start, so let's skip it.
void xfer_init(PurpleXfer* xfer);

} // End of anonymous namespace

PurpleXfer* new_xfer(PurpleConnection* gc, uint64 uid)
{
    if (uid == 0)
        return nullptr;

    PurpleXfer* xfer = purple_xfer_new(purple_connection_get_account(gc), PURPLE_XFER_SEND,
                                       buddy_name_from_uid(uid).data());

    xfer->data = new uint64(uid);

    // NOTE: We are lazy and do not implement "proper" sending file in buffer. We load the
    // contents of the file in xfer_start and hope that noone will be uploading DVD ISOs
    // to Vk.com. A proper way would be implementing xfer write_fnc.
    purple_xfer_set_init_fnc(xfer, xfer_init);

    return xfer;
}

namespace
{

// Helper function, updating xfer progress and cancelling it if user has pressed cancel.
void xfer_upload_progress(PurpleXfer* xfer, PurpleHttpConnection* http_conn, int processed, int total);
// Sends document described by v to uid.
bool send_doc(PurpleConnection* gc, uint64 uid, const picojson::value& v);

void xfer_init(PurpleXfer* xfer)
{
    assert(purple_xfer_get_type(xfer) == PURPLE_XFER_SEND);
    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));

    const char* filepath = purple_xfer_get_local_filename(xfer);
    const char* filename = purple_xfer_get_filename(xfer);
    char* contents;
    size_t size;
    if (!g_file_get_contents(filepath, &contents, &size, nullptr)) {
        purple_debug_error("prpl-vkcom", "Unable to read file %s\n", filepath);
        return;
    }

    // We manually increase reference count for xfer, so that it does not die without us noticing it.
    // Xfer can be cancelled locally anytime, which may lead to error callback getting called or not called.
    // The former happens if the user cancelled xfer before xfer_upload_progress has been called once again.
    purple_xfer_ref(xfer);
    upload_doc_for_im(gc, filename, contents, size, [=](const picojson::value& v) {
        uint64* uid = (uint64*)xfer->data;

        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
            purple_debug_info("prpl-vkcom", "Transfer has been cancelled by user\n");
        } else {
            if (send_doc(gc, *uid, v)) {
                purple_xfer_set_completed(xfer, true);
                purple_xfer_end(xfer);
            } else {
                purple_xfer_cancel_remote(xfer);
            }
        }
        delete uid;
        purple_xfer_unref(xfer);

        g_free(contents);
    }, [=] {
        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL)
            purple_debug_info("prpl-vkcom", "Transfer has been cancelled by user\n");
        else
            purple_xfer_cancel_remote(xfer);
        delete (uint64*)xfer->data;
        purple_xfer_unref(xfer);

        g_free(contents);
    }, [=](PurpleHttpConnection* http_conn, int processed, int total) {
        xfer_upload_progress(xfer, http_conn, processed, total);
    });
}

void xfer_upload_progress(PurpleXfer* xfer, PurpleHttpConnection* http_conn, int processed, int total)
{
    if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
        purple_http_conn_cancel(http_conn);
        return;
    }

    size_t xfer_size = purple_xfer_get_size(xfer);
    // xfer_size is slightly less than total due to headers and stuff, so let's compensate.
    size_t sent = (processed > int(total - xfer_size)) ? processed + xfer_size - total : 0;
    purple_xfer_set_bytes_sent(xfer, sent);
    purple_xfer_update_progress(xfer);
}

bool send_doc(PurpleConnection* gc, uint64 uid, const picojson::value& v)
{
    if (!v.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }
    const picojson::value& doc = v.get(0);
    if (!field_is_present<string>(doc, "url")) {
        purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }

    const string& doc_url = doc.get("url").get<string>();

    string attachemnt = parse_vkcom_attachments(doc_url);
    send_im_attachment(gc, uid, attachemnt);

    // Write information about uploaded file. so that user will be able to send the link to someone else.
    string who = buddy_name_from_uid(uid);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who.data(),
                                                                      purple_connection_get_account(gc));
    if (conv) {
        string message = str_format("Sent file will be permanently available at %s", doc_url.data());
        purple_conversation_write(conv, nullptr, message.data(), PURPLE_MESSAGE_SYSTEM, time(nullptr));
    }

    return true;
}

} // End of anonymous namespace
