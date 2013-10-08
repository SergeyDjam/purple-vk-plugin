// Utility functions for making HTTP requests.

#pragma once

#include "common.h"

#include "contrib/purple/http.h"

// Destroys global keepalive pool, which is used by all connections made by the plugin. Must be called
// in unload_plugin.
void destroy_keepalive_pool();

using HttpCallback = std::function<void(PurpleHttpConnection *http_conn, PurpleHttpResponse *response)>;

// Utility function: run purple_http_get with global keep-alive pool and add to global connection set.
PurpleHttpConnection* http_get(PurpleConnection *gc, const string& url, HttpCallback callback);

// Utility function: run purple_http_get with global keep-alive pool and add to global connection set.
PurpleHttpConnection* http_request(PurpleConnection* gc, PurpleHttpRequest* request, HttpCallback callback);

// A wrapper around purple_http_request, which updates url in PurpleHttpRequest. This url can be
// later retrieved inside the callback function. This differs from the standard purple_http_request
// behaviour where only url inside PurpleConnection is updated (it is inaccessible to outside code).
// Connection is run with global keep-alive pool and added to global connection set.
PurpleHttpConnection* http_request_update_on_redirect(PurpleConnection* gc, PurpleHttpRequest* request,
                                                      HttpCallback callback);

// Copy cookie-jar from already running connection to new request.
void http_request_copy_cookie_jar(PurpleHttpRequest* target, PurpleHttpConnection* source_conn);
