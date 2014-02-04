// Basic functions for uploading images/documents to Vk.com servers.

#pragma once

#include "common.h"

#include <connection.h>

#include <contrib/purple/http.h>
#include "contrib/picojson.h"

typedef std::function<void(const picojson::value& result)> UploadedCb;
typedef std::function<void(PurpleHttpConnection* http_conn, int processed, int total)> UploadProgressCb;

// Uploads document via docs.getWallUploadServer which means document will be prepared to be
// sent as attachment via im. value returned via UploadedCb call is returned from docs.save
// call.
// NOTE: contents must be valid until either uploaded_cb or error_cb is called.
void upload_doc_for_im(PurpleConnection* gc, const char* name, const void* contents, size_t size,
                       const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                       const UploadProgressCb& upload_progress_cb = nullptr);

// Uploads photo via docs.getWallUploadServer which means document will be prepared to be
// sent as attachment via im. value returned via UploadedCb is returned from photos.saveMessagesPhoto
// call.
// NOTE: contents must be valid until either uploaded_cb or error_cb is called.
void upload_photo_for_im(PurpleConnection* gc, const char* name, const void* contents, size_t size,
                         const UploadedCb& uploaded_cb, const ErrorCb& error_cb,
                         const UploadProgressCb& upload_progress_cb = nullptr);
