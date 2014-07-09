#include <debug.h>
#include <fstream>
#include <glib.h>

#include "strutils.h"
#include "trie.h"

#include "vk-smileys.h"

using std::ifstream;

#if defined(DATADIR)
const char* datadirs[] = { DATADIR };
#elif defined(__linux__)
const char* datadirs[] = { "/usr/share/pixmaps/pidgin" };
#elif defined(_WIN32)
const char* datadirs[] = { "C:\\Program Files\\Pidgin\\pixmaps",
                           "C:\\Program Files (x86)\\Pidgin\\pixmaps" }
#else
#error "DATADIR not defined"
#endif

namespace
{

// Map from text smiley to unicode version. Used when converting messages before sending.
Trie<string> ascii_to_unicode_smiley;
// Map from unicode version to "canonical" text smiley. Used when converting messages after
// receiving.
Trie<string> unicode_to_ascii_smiley;

string find_smiley_theme()
{
    string ret;
    for (const char* dir: datadirs) {
        char* path = g_build_filename(dir, "emotes", "vk", nullptr);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            ret = path;
            g_free(path);
            return ret;
        }
        g_free(path);
    }
    return ret;
}

void load_smile_theme(const string& theme_dir)
{
    char* theme_path = g_build_filename(theme_dir.data(), "theme", nullptr);
    ifstream theme_file;
    theme_file.open(theme_path);
    if (!theme_file.is_open()) {
        vkcom_debug_error("Unable to open theme file %s\n", theme_path);
        g_free(theme_path);
        return;
    }
    vkcom_debug_info("Parsing theme file %s\n", theme_path);

    string buf;
    bool found_section = false;
    while (std::getline(theme_file, buf)) {
        str_trim(buf);
        if (buf.length() == 0)
            continue;

        if (buf[0] == '[') {
            found_section = true;
            continue;
        }

        if (found_section) {
            vector<string> v; // v = { filename, smiley shortcut, [smile shortcut 2, ...] }
            str_split_append(buf, ' ', v);
            if (v.size() <= 1) {
                vkcom_debug_error("Strange line in emotes theme file %s: %s\n", theme_path,
                                  buf.data());
                continue;
            }
//            char* smiley_file = g_build_filename(theme_dir.data(), v[0].data(), nullptr);
//            g_free(smiley_file);

            // Find the unicode and first ASCII version of smiley.
            string ascii_version;
            string unicode_version;
            for (size_t i = 1; i < v.size(); i++) {
                // Check if any chars are >= 128
                bool is_unicode = false;
                for (char c: v[i]) {
                    if (c < 0) {
                        is_unicode = true;
                        break;
                    }
                }
                if (is_unicode) {
                    if (unicode_version.empty())
                        unicode_version = v[i];
                } else {
                    if (ascii_version.empty())
                        ascii_version = v[i];
                }
            }

            if (unicode_version.empty()) {
                vkcom_debug_error("Strange line in emotes theme file %s, does not contain a unicode"
                                  " version: %s\n", theme_path, buf.data());
                continue;
            }
            for (size_t i = 1; i < v.size(); i++) {
                if (v[i] != unicode_version)
                    ascii_to_unicode_smiley.insert(v[i].data(), unicode_version);
            }

            if (!ascii_version.empty())
                unicode_to_ascii_smiley.insert(unicode_version.data(), ascii_version);
        }
    }
    g_free(theme_path);
}

} // namespace

void initialize_smileys()
{
    string theme_dir = find_smiley_theme();
    if (theme_dir.empty()) {
        vkcom_debug_error("Unable to find vk smileys theme, did you install plugin properly?\n");
        return;
    }
    load_smile_theme(theme_dir);
}

namespace
{

bool str_at_isspace(const string& s, size_t i)
{
    if (i == s.length())
        return true;
    return ascii_isspace(s[i]);
}

}

void convert_outgoing_smileys(string& message)
{
    for (size_t index = 0; index < message.length();) {
        int ascii_len;
        string* unicode = ascii_to_unicode_smiley.match(message.data() + index, &ascii_len);
        if (!unicode) {
            index++;
            continue;
        }
        // Check that there are spaces before and after the smiley. Otherwise, it is very easy
        // to mix it with normal text, e.g. parse 8) in "12345678)" or :( in "like that:(some text)"
        if (!(index == 0 || str_at_isspace(message, index - 1))
                || !str_at_isspace(message, index + ascii_len)) {
            index++;
            continue;
        }

        message.replace(index, ascii_len, *unicode);
        index += unicode->length();
    }
}

void convert_incoming_smileys(string& message)
{
    for (size_t index = 0; index < message.length();) {
        int match_len;
        string* ascii = unicode_to_ascii_smiley.match(message.data() + index, &match_len);
        if (!ascii) {
            index++;
            continue;
        }

        message.replace(index, match_len, *ascii);
        index += ascii->length();
    }
}
