// Miscellaneous utilities

#pragma once

#include <map>

using std::pair;
using std::map;

#include "common.h"

#include <connection.h>
#include <libxml/tree.h>

#include <contrib/picojson/picojson.h>
#include <contrib/purple/http.h>

// A nicer wrapper around xmlGetProp
string get_xml_node_prop(xmlNode* node, const char* tag, const char* default_value = "");

// Returns an x-www-form-urlencoded representation of a set of parameters.
string urlencode_form(const map<string, string>& params);
string urlencode_form(const vector<pair<string, string>>& params);

// Returns mapping key -> value from urlencoded form.
map<string, string> parse_urlencoded_form(const char* encoded);

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

// A tiny wrapper around purple_unescape_html, accepting and returning string.
string unescape_html(const char* text);
string unescape_html(const string& text);

// Returns path to data directory (usually /usr/share for Linux, C:\Program Files\Pidgin
// for Windows).
string get_data_dir();
