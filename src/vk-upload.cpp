#include <gio/gio.h>
#include <random>

#include "httputils.h"
#include "miscutils.h"
#include "vk-api.h"

#include "vk-upload.h"


namespace
{

// Helper function, which is used by upload_doc and upload_photo.
void upload_file(PurpleConnection* gc, const char* get_upload_server, const char* partname, const char* name,
                 const void* contents, size_t size, const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                 const UploadProgressCb& upload_progress_cb = nullptr);

} // End of anonymous namespace

void upload_doc_for_im(PurpleConnection* gc, const char* name, const void* contents, size_t size,
                       const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                       const UploadProgressCb& upload_progress_cb)
{
    vkcom_debug_info("Uploading document for IM\n");

    upload_file(gc, "docs.getWallUploadServer", "file", name, contents, size, [=](const picojson::value& v) {
        if (!field_is_present<string>(v, "file")) {
            vkcom_debug_error("Strange response from upload server: %s\n", v.serialize().data());
            if (error_cb)
                error_cb();
            return;
        }

        const string& file = v.get("file").get<string>();
        CallParams params = { {"file", file} };
        vk_call_api(gc, "docs.save", params, [=](const picojson::value& result) {
            uploaded_cb(result);
        }, [=](const picojson::value&) {
            if (error_cb)
                error_cb();
        });
    }, error_cb, upload_progress_cb);
}

void upload_photo_for_im(PurpleConnection* gc, const char* name, const void* contents, size_t size,
                         const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                         const UploadProgressCb& upload_progress_cb)
{
    vkcom_debug_info("Uploading photo for IM\n");

    upload_file(gc, "photos.getMessagesUploadServer", "photo", name, contents, size, [=](const picojson::value& v) {
        if (!(field_is_present<int>(v, "server") || field_is_present<string>(v, "server"))
            || !field_is_present<string>(v, "photo") || !field_is_present<string>(v, "hash")) {
            vkcom_debug_error("Strange response from upload server: %s\n", v.serialize().data());
            if (error_cb)
                error_cb();
            return;
        }

        const string& server = v.get("server").to_str();
        const string& photo = v.get("photo").get<string>();
        const string& hash = v.get("hash").get<string>();
        CallParams params = { {"server", server}, {"photo", photo}, {"hash", hash} };
        vk_call_api(gc, "photos.saveMessagesPhoto", params, [=](const picojson::value& result) {
            uploaded_cb(result);
        }, [=](const picojson::value&) {
            if (error_cb)
                error_cb();
        });
    }, error_cb, upload_progress_cb);
}

namespace
{

// Initiates HTTP transfer to upload_url.
void start_upload(PurpleConnection* gc, const string& upload_url, const char* partname, const char* name,
                  const void* contents, size_t size, const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                  const UploadProgressCb& upload_progress_cb);
// Prepares HTTP POST request with multipart/form-data with partname, containing given contents.
PurpleHttpRequest* prepare_upload_request(const string& url, const char* partname, const void* contents,
                                          size_t size, const char* name);
// Generates random boundary string for multipart/form-data POST requests.
string generate_boundary();

// Helper function which calls upload_progress_cb
void progress_watcher(PurpleHttpConnection* http_conn, gboolean reading_state, int processed, int total,
                      void* progress_data);

void upload_file(PurpleConnection* gc, const char* get_upload_server, const char* partname, const char* name,
                 const void* contents, size_t size, const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                 const UploadProgressCb& upload_progress_cb)
{
    vk_call_api(gc, get_upload_server, CallParams(), [=](const picojson::value& result) {
        if (!field_is_present<string>(result, "upload_url")) {
            vkcom_debug_error("Strange response from %s: %s\n", get_upload_server, result.serialize().data());

            if (error_cb)
                error_cb();
            return;
        }
        const string& upload_url = result.get("upload_url").get<string>();
        vkcom_debug_info("Uploading to %s\n", upload_url.data());

        start_upload(gc, upload_url, partname, name, contents, size, uploaded_cb, error_cb, upload_progress_cb);
    }, [=](const picojson::value&) {
        if (error_cb)
            error_cb();
    });
}

void start_upload(PurpleConnection* gc, const string& upload_url, const char* partname, const char* name,
                  const void* contents, size_t size, const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                  const UploadProgressCb& upload_progress_cb)
{
    vkcom_debug_info("Starting upload\n");

    PurpleHttpRequest* request = prepare_upload_request(upload_url, partname, contents, size, name);
    UploadProgressCb* progress_data = nullptr;
    if (upload_progress_cb)
        progress_data = new UploadProgressCb(upload_progress_cb);

    PurpleHttpConnection* http_conn = http_request(gc, request,
    [=](PurpleHttpConnection*, PurpleHttpResponse* response) {
        delete progress_data;

        if (!purple_http_response_is_successful(response)) {
            if (error_cb)
                error_cb();
            return;
        }

        const char* response_text = purple_http_response_get_data(response, nullptr);
        const char* response_text_copy = response_text; // Picojson updates iterators it received.
        picojson::value root;
        string error = picojson::parse(root, response_text, response_text + strlen(response_text));
        if (!error.empty()) {
            vkcom_debug_error("Error parsing %s: %s\n", response_text_copy, error.data());
            if (error_cb)
                error_cb();
            return;
        }

        vkcom_debug_info("Finished upload\n");

        uploaded_cb(root);
    });
    purple_http_request_unref(request);
    purple_http_conn_set_progress_watcher(http_conn, progress_watcher, progress_data, -1);
}

PurpleHttpRequest* prepare_upload_request(const string& url, const char* partname, const void* contents,
                                          size_t size, const char* name)
{
    PurpleHttpRequest* request = purple_http_request_new(url.data());
    purple_http_request_set_method(request, "POST");

    string boundary;
    while (true) {
        boundary = generate_boundary();
        // Check if boundary is not present in the contents.
        if (!g_strstr_len((const char*)contents, size, boundary.data()))
            break;
    }

    purple_http_request_header_set_printf(request, "Content-type", "multipart/form-data; boundary=%s",
                                          boundary.data());

    char* content_type = g_content_type_guess(name, nullptr, 0, nullptr);
    char* mime_type;
    if (content_type)
        mime_type = g_content_type_get_mime_type(content_type);
    else
        mime_type = g_strdup("application/octet-stream");
    g_free(content_type);

    vkcom_debug_info("Sending file %s with size %zu and mime-type %s to %s\n", name, size,
                     mime_type, url.data());
    string body_header = str_format("--%s\r\n"
                                    "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %zu\r\n"
                                    "\r\n", boundary.data(), partname, name, mime_type, size);
    string body_footer = str_format("\r\n--%s--", boundary.data());
    g_free(mime_type);

    // Yes, we copy file contents in memory, let's hope again, that user will not be uploading large files.
    // Anyway, we will have to add compression to zip on-the-fly for most of the file types later.
    // Another copy happens in set_contents, so it's a good thing we capped the size of the files
    // to 150mb!
    //
    // TODO: Add properly this stuff via purple_http_request_set_contents_reader.
    vector<char> body;
    body.reserve(body_header.size() + size + body_footer.size());
    append(body, body_header);
    const char* char_contents = (const char*)contents;
    body.insert(body.end(), char_contents, char_contents + size);
    append(body, body_footer);
    body.insert(body.end(), body_footer.begin(), body_footer.end());

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

void progress_watcher(PurpleHttpConnection* http_conn, gboolean reading_state, int processed, int total,
                      void* progress_data)
{
    if (!progress_data)
        return;
    if (reading_state)
        return;

    vkcom_debug_info("Uploaded %d %d\n", processed, total);

    UploadProgressCb* upload_progress_cb = (UploadProgressCb*)progress_data;
    (*upload_progress_cb)(http_conn, processed, total);
}

} // End of anonymous namespace
