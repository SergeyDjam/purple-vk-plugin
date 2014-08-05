#include <debug.h>

#include "contutils.h"
#include "strutils.h"

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

PurpleXfer* new_xfer(PurpleConnection* gc, uint64 user_id)
{
    if (user_id == 0)
        return nullptr;

    string name = user_name_from_id(user_id);
    PurpleXfer* xfer = purple_xfer_new(purple_connection_get_account(gc), PURPLE_XFER_SEND, name.data());

    xfer->data = new uint64(user_id);
    // NOTE: We are lazy and do not implement "proper" sending file in buffer. We load the
    // contents of the file in xfer_start. A proper way would be implementing xfer write_fnc.
    purple_xfer_set_init_fnc(xfer, xfer_init);

    return xfer;
}

namespace
{

// Returns string, containing md5sum of contents.
string compute_md5sum(const char* contents, gsize size)
{
    char* str = g_compute_checksum_for_data(G_CHECKSUM_MD5, (const unsigned char*)contents, size);
    string ret = str;
    g_free(str);
    return ret;
}

// Helper function, updating xfer progress and cancelling it if user has pressed cancel.
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

// Sends given document to user and writes into the conversation about it.
// If resend is true, this url has already been sent.
void send_doc_url(PurpleConnection* gc, uint64 user_id, const string& url, bool resend)
{
    string attachemnt = parse_vkcom_attachments(url);
    send_im_attachment(gc, user_id, attachemnt);

    // Write information about uploaded file. so that user will be able to send the link to someone else.
    string who = user_name_from_id(user_id);
    PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
                                                                     who.data(),
                                                                     purple_connection_get_account(gc));
    if (conv) {
        string message;
        if (resend)
            message = str_format(i18n("Sent file has already been uploaded and is permanently"
                                      " available at %s"), url.data());
        else
            message = str_format(i18n("Sent file will be permanently available at %s"), url.data());
        purple_conversation_write(conv, nullptr, message.data(), PURPLE_MESSAGE_SYSTEM,
                                  time(nullptr));
    }
}

// Sends document described by v to user_id and save doc to uploaded_docs.
bool send_doc(PurpleConnection* gc, uint64 user_id, const VkUploadedDocInfo& doc, const picojson::value& v)
{
    if (!v.is<picojson::array>()) {
        vkcom_debug_error("Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }
    const picojson::value& d = v.get(0);
    if (!field_is_present<string>(d, "url")) {
        vkcom_debug_error("Strange response from docs.save: %s\n", v.serialize().data());
        return false;
    }

    const string& doc_url = d.get("url").get<string>();
    send_doc_url(gc, user_id, doc_url, false);

    // Store the uploaded document.
    uint64 doc_id = d.get("id").get<double>();
    VkData& gc_data = get_data(gc);
    gc_data.uploaded_docs[doc_id] = doc;
    gc_data.uploaded_docs[doc_id].url = doc_url;

    return true;
}

// Destructor for xfer.
void xfer_fini(PurpleXfer* xfer, char* contents)
{
    delete (uint64*)xfer->data;
    purple_xfer_unref(xfer);

    g_free(contents);
}

// Uploads document and sends it.
void start_uploading_doc(PurpleConnection* gc, PurpleXfer* xfer, const VkUploadedDocInfo& doc, char* contents)
{
    upload_doc_for_im(gc, doc.filename.data(), contents, doc.size, [=](const picojson::value& v) {
        uint64 user_id = *(uint64*)xfer->data;

        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
            vkcom_debug_info("Transfer has been cancelled by user\n");
        } else {
            if (send_doc(gc, user_id, doc, v)) {
                purple_xfer_set_completed(xfer, true);
                purple_xfer_end(xfer);
            } else {
                purple_xfer_cancel_remote(xfer);
            }
        }
        xfer_fini(xfer, contents);
    }, [=] {
        if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL)
            vkcom_debug_info("Transfer has been cancelled by user\n");
        else
            purple_xfer_cancel_remote(xfer);
        xfer_fini(xfer, contents);
    }, [=](PurpleHttpConnection* http_conn, int processed, int total) {
        xfer_upload_progress(xfer, http_conn, processed, total);
    });
}

// Calls docs.get for the current user and removes all the docs from uploaded_docs, which do not exist
// or do not match the stored parameters.
void clean_nonexisting_docs(PurpleConnection* gc, const SuccessCb& success_cb)
{
    vkcom_debug_info("Checking for stale information about uploaded documents\n");

    // A set of document ids, which are still available.
    shared_ptr<set<uint64>> existing_doc_ids{ new set<uint64>() };

    vk_call_api_items(gc, "docs.get", CallParams(), true, [=](const picojson::value& v) {
        if (!field_is_present<double>(v, "id") || !field_is_present<string>(v, "title")
                || !field_is_present<double>(v, "size") || !field_is_present<string>(v, "url")) {
            vkcom_debug_error("Strange response from docs.get: %s\n", v.serialize().data());
            return;
        }

        uint64 doc_id = v.get("id").get<double>();

        VkData& gc_data = get_data(gc);
        if (contains(gc_data.uploaded_docs, doc_id)) {
            const VkUploadedDocInfo& doc = gc_data.uploaded_docs[doc_id];

            const string& title = v.get("title").get<string>();
            uint64 size = v.get("size").get<double>();
            const string& url = v.get("url").get<string>();

            if (doc.filename == title && doc.size == size && doc.url == url)
                existing_doc_ids->insert(doc_id);
            else
                vkcom_debug_info("Document %llu changed either title, size or url, "
                                  "removing from uploaded\n", (unsigned long long)doc_id);
        }
    }, [=]() {
        VkData& gc_data = get_data(gc);
        int size_diff = gc_data.uploaded_docs.size() - existing_doc_ids->size();
        if (size_diff > 0)
            vkcom_debug_info("%d docs removed from uploaded\n", size_diff);

        erase_if(gc_data.uploaded_docs, [=](const pair<uint64, VkUploadedDocInfo>& p) {
            uint64 doc_id = p.first;
            return !contains(*existing_doc_ids, doc_id);
        });

        if (success_cb)
            success_cb();
    }, [=](const picojson::value& v) {
        vkcom_debug_error("Error in docs.get: %s, removing all info on uploaded docs\n",
                          v.serialize().data());
        get_data(gc).uploaded_docs.clear();

        if (success_cb)
            success_cb();
    });
}

// Either finds matching doc, checks that it exists and sends it or uploads new doc.
void find_or_upload_doc(PurpleConnection* gc, PurpleXfer* xfer, const VkUploadedDocInfo& doc, char* contents)
{
    // We have a concurrency problem here: if the document is uploaded and added during the
    // call to clean_nonexisting_docs (between calling docs.get and parsing the results) it will
    // not be added to uploaded_docs. It is a minor problem (the document will be reuploaded
    // the next time it is added) and all this "check if doc still exists" approach is
    // non-concurrency-proof already.
    clean_nonexisting_docs(gc, [=] {
        for (const pair<uint64, VkUploadedDocInfo>& p: get_data(gc).uploaded_docs) {
            uint64 doc_id = p.first;
            const VkUploadedDocInfo& updoc = p.second;
            if (updoc.filename == doc.filename && updoc.size == doc.size
                    && updoc.md5sum == doc.md5sum) {
                vkcom_debug_info("Filename, size and md5sum matches the doc %llu, resending it.\n",
                                 (unsigned long long)doc_id);

                uint64 user_id = *(uint64*)xfer->data;
                send_doc_url(gc, user_id, updoc.url, true);

                purple_xfer_set_completed(xfer, true);
                purple_xfer_end(xfer);
                xfer_fini(xfer, contents);
                return;
            }
        }

        start_uploading_doc(gc, xfer, doc, contents);
    });
}

void xfer_init(PurpleXfer* xfer)
{
    assert(purple_xfer_get_type(xfer) == PURPLE_XFER_SEND);
    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));

    // We manually increase reference count for xfer, so that it does not die without us noticing it.
    // Xfer can be cancelled locally anytime, which may lead to error callback getting called or not called.
    // The former happens if the user cancelled xfer before xfer_upload_progress has been called once again.
    purple_xfer_ref(xfer);

    const char* filepath = purple_xfer_get_local_filename(xfer);
    const char* filename = purple_xfer_get_filename(xfer);

    vkcom_debug_info("Reading file contents\n");

    // Load all contents in memory.
    char* contents;
    gsize size;
    if (!g_file_get_contents(filepath, &contents, &size, nullptr)) {
        vkcom_debug_error("Unable to read file %s\n", filepath);

        purple_xfer_cancel_local(xfer);
        xfer_fini(xfer, nullptr);
        return;
    }

    if (size > (gsize)MAX_UPLOAD_SIZE) {
        vkcom_debug_info("Unable to upload files larger than %d\n", MAX_UPLOAD_SIZE);

        purple_xfer_cancel_remote(xfer);
        xfer_fini(xfer, nullptr);
        return;
    }

    vkcom_debug_info("Successfully read file contents\n");

    VkUploadedDocInfo doc;
    doc.filename = filename;
    doc.size = size;
    doc.md5sum = compute_md5sum(contents, size);

    find_or_upload_doc(gc, xfer, doc, contents);
}

} // End of anonymous namespace
