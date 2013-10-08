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
using TimeoutCb = std::function<bool()>;
void timeout_add(PurpleConnection* gc, unsigned milliseconds, TimeoutCb callback);

// Removes all timed events, added with timeout_add, associated with gc.
void timeout_remove_all(PurpleConnection* gc);

// A tiny wrapper around purple_unescape_html, accepting and returning string.
string unescape_html(const char* text);
string unescape_html(const string& text);
