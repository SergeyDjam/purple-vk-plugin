// Miscellaneous utilities

#pragma once

#include "common.h"

#include <connection.h>
#include <libxml/tree.h>

#include "contrib/purple/http.h"
#include "contrib/picojson.h"

// A nicer wrapper around xmlGetProp
string get_xml_node_prop(xmlNode* node, const char* tag, const char* default_value = "");

// Returns an x-www-form-urlencoded representation of a set of parameters.
string urlencode_form(const string_map& params);
string urlencode_form(const vector<string_pair>& params);

// Returns mapping key -> value from urlencoded form.
string_map parse_urlencoded_form(const char* encoded);

// A rather specific function, which returns the length of prefix (the first part of the string),
// so that when it is encoded, it is no longer than max_urlencoded_len.
// Used when you have large inputs and want to split them.
const size_t MAX_URLENCODED_STRING = 1900;
size_t max_urlencoded_prefix(const char* s, size_t max_urlencoded_len = MAX_URLENCODED_STRING);


// Checks if JSON value is an object, contains key and the type of value for that key is T.
template<typename T>
bool field_is_present(const picojson::value& v, const string& key)
{
    if (!v.is<picojson::object>())
        return false;
    if (!v.contains(key))
        return false;
    if (!v.get(key).is<T>())
        return false;
    return true;
}


// A g_timeout_add wrapper, accepting std::function.
typedef std::function<bool()> TimeoutCb;
void timeout_add(PurpleConnection* gc, unsigned milliseconds, TimeoutCb callback);

// Removes all timed events, added with timeout_add, associated with gc.
void timeout_remove_all(PurpleConnection* gc);

// A tiny wrapper around purple_unescape_html, accepting and returning string.
string unescape_html(const char* text);
string unescape_html(const string& text);

// Checks if url starts with http:// and replaces it with https://
string ensure_https_url(const string& url);
