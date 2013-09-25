#include "vk-filexfer.h"

#include <debug.h>
#include <gio/gio.h>
#include <random>

#include "httputils.h"
#include "vk-api.h"
#include "vk-common.h"
#include "vk-message.h"
#include "utils.h"

namespace
{

// Called upon completion of upload. doc_url contains full URL to the uploaded document.
using UploadCompletedCb = std::function<void(const string& doc_url)>;

struct XferData
{
public:
    XferData(PurpleXfer* xfer, const UploadCompletedCb& completed_cb)
        : m_xfer(xfer),
          m_completed_cb(completed_cb)
    {
        // NOTE: We want to PurpleXfer and corresponding XferData to have the same lifetimes. XferData
        // must be destroyed either via cancel(), cancel_remote(), completed()
        purple_xfer_ref(m_xfer);
    }

    // Checks if xfer has been cancelled. In this case, all further operations should be cancelled
    // and XferData should be deleted.
    bool is_cancelled_locally() const
    {
        return purple_xfer_get_status(m_xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL;
    }

    void cancel()
    {
        assert(is_cancelled_locally());
        delete this;
    }

    // An error has occured during transfer, cancel xfer and delete.
    void cancel_remote()
    {
        purple_xfer_cancel_remote(m_xfer);
        delete this;
    }

    // Call completion callback and destroy this.
    void completed(const string& doc_url)
    {
        if (m_completed_cb)
            m_completed_cb(doc_url);
        purple_xfer_set_completed(m_xfer, true);
        purple_xfer_end(m_xfer);
        delete this;
    }

private:
    PurpleXfer* m_xfer;

    UploadCompletedCb m_completed_cb;

    ~XferData()
    {
        purple_xfer_unref(m_xfer);
    }
};

// Starts xfer. Finds out upload server URL, creates the full request for upload server and writes it.
// There seems to be no reason to call purple_xfer_start, so let's skip it.
void xfer_init(PurpleXfer* xfer);

// Sends message with document with url doc_url as an attachment to uid.
void send_doc_with_url(PurpleConnection* gc, uint64 uid, const string& doc_url);

} // End of anonymous namespace

PurpleXfer* new_xfer(PurpleConnection* gc, uint64 uid)
{
    PurpleXfer* xfer = purple_xfer_new(purple_connection_get_account(gc), PURPLE_XFER_SEND,
                                       buddy_name_from_uid(uid).c_str());

    XferData* xfer_data = new XferData(xfer, [=](const string& doc_url) {
        send_doc_with_url(gc, uid, doc_url);
    });
    xfer->data = xfer_data;

    // NOTE: We are lazy and do not implement "proper" sending file in buffer. We load the
    // contents of the file in xfer_start and hope that noone will be uploading DVD ISOs
    // to Vk.com. A proper way would be implementing xfer write_fnc.
    purple_xfer_set_init_fnc(xfer, xfer_init);

    return xfer;
}

namespace
{

// Upload file as document via upload_url.
void upload_doc(PurpleXfer* xfer, const string& upload_url);

void xfer_init(PurpleXfer* xfer)
{
    assert(purple_xfer_get_type(xfer) == PURPLE_XFER_SEND);
    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));

    XferData* xfer_data = (XferData*)xfer->data;
    vk_call_api(gc, "docs.getWallUploadServer", {}, [=](const picojson::value& result) {
        if (xfer_data->is_cancelled_locally()) {
            xfer_data->cancel();
            return;
        }

        if (!field_is_present<string>(result, "upload_url")) {
            purple_debug_error("prpl-vkcom", "Strange response from docs.getWallUploadServer: %s\n",
                               result.serialize().c_str());
            xfer_data->cancel_remote();
            return;
        }
        const string& upload_url = result.get("upload_url").get<string>();
        purple_debug_info("prpl-vkcom", "Uploading to %s\n", upload_url.c_str());

        upload_doc(xfer, upload_url);
    }, [=](const picojson::value&) {
        xfer_data->cancel_remote();
    });
}

// Prepares HTTP POST request with multipart/form-data with "file" part, containing contents
// of filename.
PurpleHttpRequest* prepare_upload_request(const string& url, const char* filepath, const char* filename);

// Helper function, updating xfer progress and cancelling it if user has pressed cancel.
void xfer_progress_watcher(PurpleHttpConnection* http_conn, gboolean reading_state, int processed, int total,
                           void* user_data);

// Saves document and finishes (or cancels) the xfer.
void save_doc(PurpleXfer* xfer, const string& file);

// Upload file as document via upload_url.
void upload_doc(PurpleXfer* xfer, const string& upload_url)
{
    PurpleHttpRequest* request = prepare_upload_request(upload_url, purple_xfer_get_local_filename(xfer),
                                                        purple_xfer_get_filename(xfer));
    XferData* xfer_data = (XferData*)xfer->data;
    if (!request) {
        purple_debug_error("prpl-vkcom", "Unable to read %s\n", purple_xfer_get_local_filename(xfer));
        xfer_data->cancel_remote();
        return;
    }

    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));
    PurpleHttpConnection* http_conn = http_request(gc, request,
        [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        if (!purple_http_response_is_successful(response)) {
            // Check if user cancelled transmission.
            if (xfer_data->is_cancelled_locally()) {
                purple_debug_info("prpl-vkcom", "Transfer has been cancelled by user\n");
                xfer_data->cancel();
                return;
            } else {
                purple_debug_error("prpl-vkcom", "Strange response from upload server: %s\n",
                                   purple_http_response_get_error(response));
                xfer_data->cancel_remote();
                return;
            }
        }

        const char* response_text = purple_http_response_get_data(response, nullptr);
        const char* response_text_copy = response_text; // Picojson updates iterators it received.
        picojson::value root;
        string error = picojson::parse(root, response_text, response_text + strlen(response_text));
        if (!error.empty()) {
            purple_debug_error("prpl-vkcom", "Error parsing %s: %s\n", response_text_copy, error.c_str());
            xfer_data->cancel_remote();
            return;
        }
        if (!field_is_present<string>(root, "file")) {
            purple_debug_error("prpl-vkcom", "Strange response from upload server: %s\n", response_text_copy);
            xfer_data->cancel_remote();
            return;
        }
        const string& file = root.get("file").get<string>();
        save_doc(xfer, file);
    });
    purple_http_request_unref(request);
    purple_http_conn_set_progress_watcher(http_conn, xfer_progress_watcher, xfer, -1);
}

void xfer_progress_watcher(PurpleHttpConnection* http_conn, gboolean reading_state, int processed, int total,
                           void* user_data)
{
    PurpleXfer* xfer = (PurpleXfer*)user_data;
    if (purple_xfer_get_status(xfer) == PURPLE_XFER_STATUS_CANCEL_LOCAL) {
        purple_http_conn_cancel(http_conn);
        return;
    }

    // We are uploading, no interest in reading puny response.
    if (reading_state)
        return;

    size_t xfer_size = purple_xfer_get_size(xfer);
    // xfer_size is slightly less than total due to headers and stuff, so let's compensate.
    size_t sent = (processed > int(total - xfer_size)) ? processed + xfer_size - total : 0;
    purple_xfer_set_bytes_sent(xfer, sent);
    purple_xfer_update_progress(xfer);
}

// Generates random boundary string for multipart/form-data POST requests.
string generate_boundary();

PurpleHttpRequest* prepare_upload_request(const string& url, const char* filepath, const char* filename)
{
    PurpleHttpRequest* request = purple_http_request_new(url.c_str());
    purple_http_request_set_method(request, "POST");

    string boundary = generate_boundary();
    purple_http_request_header_set_printf(request, "Content-type", "multipart/form-data; boundary=%s",
                                          boundary.c_str());

    // Read file contents.
    char* contents;
    size_t length;
    if (!g_file_get_contents(filepath, &contents, &length, nullptr))
        return nullptr;

    char* content_type = g_content_type_guess(filename, nullptr, 0, nullptr);
    char* mime_type;
    if (content_type)
        mime_type = g_content_type_get_mime_type(content_type);
    else
        mime_type = g_strdup("application/octet-stream");
    string body_header = str_format("--%s\r\n"
                                    "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %d\r\n"
                                    "\r\n", boundary.c_str(), filename, mime_type, length);
    string body_footer = str_format("\r\n--%s--", boundary.c_str());
    g_free(mime_type);
    g_free(content_type);

    vector<char> body;
    body.reserve(body_header.size() + length + body_footer.size());
    body.insert(body.end(), body_header.begin(), body_header.end());
    body.insert(body.end(), contents, contents + length);
    body.insert(body.end(), body_footer.begin(), body_footer.end());
    g_free(contents);

    // Set an hour timeout, so that we never timeout anyway.
    purple_http_request_set_timeout(request, 3600);
    purple_http_request_set_contents(request, body.data(), body.size());

    return request;
}

string generate_boundary()
{
    static std::random_device rd;
    static std::default_random_engine re(rd());
    static const char ascii_chars[] = "-_1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    string ret;
    ret.reserve(48);
    for (int i = 0; i < 48; i++)
        ret += ascii_chars[re() % (sizeof(ascii_chars) - 1)];
    return ret;
}

void save_doc(PurpleXfer* xfer, const string& file)
{
    PurpleConnection* gc = purple_account_get_connection(purple_xfer_get_account(xfer));
    XferData* xfer_data = (XferData*)xfer->data;

    CallParams params = { {"file", file} };
    vk_call_api(gc, "docs.save", params, [=](const picojson::value& v) {
        if (!v.is<picojson::array>()) {
            purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().c_str());
            xfer_data->cancel_remote();
            return;
        }
        const picojson::value& doc = v.get(0);
        if (!field_is_present<string>(doc, "url")) {
            purple_debug_error("prpl-vkcom", "Strange response from docs.save: %s\n", v.serialize().c_str());
            xfer_data->cancel_remote();
            return;
        }

        const string& doc_url = doc.get("url").get<string>();
        xfer_data->completed(doc_url);
    }, [=](const picojson::value& error) {
        purple_debug_error("prpl-vkcom", "Received an error from docs.save: %s\n", error.serialize().c_str());
        xfer_data->cancel_remote();
    });
}

// Parses doc_url. Returns false if parsing failed. We expect doc_url to be in the form
// http://domain/doc{doc_id}?...&hash={hash}?...
bool parse_doc_url(const string& doc_url, string& doc_id, string& hash)
{
    size_t start = doc_url.find("/doc");
    if (start == string::npos)
        return false;
    start++; // Skip '/'

    size_t end = doc_url.find('?', start);
    if (end == string::npos)
        return false;
    doc_id = doc_url.substr(start, end - start);
    if (doc_id.find("_") == string::npos)
        return false;

    start = doc_url.find("hash=", end);
    if (start == string::npos)
        return false;
    start += 5; // Skip 'hash='

    end = doc_url.find("&", start);
    if (end == string::npos)
        return false;
    hash = doc_url.substr(start, end - start);
    return true;
}

void send_doc_with_url(PurpleConnection* gc, uint64 uid, const string& doc_url)
{
    string doc_id;
    string hash;

    // NOTE: We have to parse doc_url because Vk.com does not send us access_key for the document.
    if (!parse_doc_url(doc_url, doc_id, hash)) {
        purple_debug_error("prpl-vkcom", "Strange doc url: %s\n", doc_url.c_str());
        // We send a message with plaintext doc URL as a last resort.
        send_im_message(gc, uid, doc_url.c_str());
        return;
    }

    string attachment = str_format("%s_%s", doc_id.c_str(), hash.c_str());
    send_im_attachment(gc, uid, attachment);
}

} // End of anonymous namespace
