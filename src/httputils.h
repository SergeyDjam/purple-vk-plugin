// Utility functions for making HTTP requests.

#pragma once

#include "common.h"

#include "contrib/purple/http.h"

// Destroys keepalive pool for PurpleConnection. Must be called on logout.
void destroy_keepalive_pool(PurpleConnection* gc);

typedef function_ptr<void(PurpleHttpConnection *http_conn, PurpleHttpResponse *response)> HttpCallback;

// Utility function: run purple_http_get with keep-alive pool and add to connection set.
PurpleHttpConnection* http_get(PurpleConnection *gc, const string& url, const HttpCallback& callback);

// Utility function: run purple_http_get with keep-alive pool and add to connection set.
PurpleHttpConnection* http_request(PurpleConnection* gc, PurpleHttpRequest* request,
                                   const HttpCallback& callback);

// A wrapper around purple_http_request, which updates url in PurpleHttpRequest. This url can be
// later retrieved inside the callback function. This differs from the standard purple_http_request
// behaviour where only url inside PurpleConnection is updated (it is inaccessible to outside code).
// Connection is run with keep-alive pool and added to connection set.
PurpleHttpConnection* http_request_update_on_redirect(PurpleConnection* gc, PurpleHttpRequest* request,
                                                      const HttpCallback& callback);

// Copy cookie-jar from already running connection to new request.
void http_request_copy_cookie_jar(PurpleHttpRequest* target, PurpleHttpConnection* source_conn);
