#include <fstream>
#include <glib.h>
#include <util.h>

#include "strutils.h"
#include "trie.h"

#include "miscutils.h"

#include "vk-smileys.h"

using std::ifstream;

namespace
{

// Map from text smiley to unicode version. Used when converting messages before sending.
Trie<string> ascii_to_unicode_smiley;
// Map from unicode version to "canonical" text smiley. Used when converting messages after
// receiving. Ascii smileys ARE escaped.
Trie<string> unicode_to_ascii_smiley;
// Map from smiley to smiley image.
typedef vector<char> SmileyImage;
Trie<shared_ptr<SmileyImage>> smiley_images;

string find_smiley_theme()
{
    char* path = g_build_filename(get_data_dir().data(), "pixmaps", "pidgin", "emotes", "vk",
                                  nullptr);
    string ret = path;
    g_free(path);

    vkcom_debug_info("Trying to find smiley theme in %s\n", ret.data());
    if (!g_file_test(ret.data(), G_FILE_TEST_IS_DIR))
        ret.clear();
    return ret;
}

bool smiley_in_default_theme(const string& smiley)
{
    return smiley == ":-)" || smiley == ":-D" || smiley == ":-(" || smiley == ";-)"
            || smiley == ":-*" || smiley == "8-)" || smiley == ":'(" || smiley == "O:-)"
            || smiley == ":-X";
}

bool load_file_contents(const char* path, vector<char>* contents)
{
    ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        vkcom_debug_error("Error opening file %s\n", path);
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t length = file.tellg();
    contents->resize(length);

    file.seekg(0);
    file.read(contents->data(), length);
    if (file.fail()) {
        vkcom_debug_error("Error reading file %s: %s\n", path, strerror(errno));
        return false;
    }

    return true;
}

void process_theme_smiley_line(const vector<string>& v, const string& buf, const char* theme_dir,
                               const char* theme_path)
{
    // We do not want to set images for custom smileys, which are already present in default
    // theme (and probably all other themes).
    bool load_smiley_image = true;
    for (size_t i = 1; i < v.size(); i++) {
        if (smiley_in_default_theme(v[i])) {
            load_smiley_image = false;
            break;
        }
    }

    if (load_smiley_image) {
        char* smiley_file_path = g_build_filename(theme_dir, v[0].data(), nullptr);
        shared_ptr<SmileyImage> image(new SmileyImage);
        if (load_file_contents(smiley_file_path, image.get())) {
            for (size_t i = 1; i < v.size(); i++)
                smiley_images.insert(v[i].data(), image);
        } else {
            vkcom_debug_error("Unable to load smiley image %s\n", smiley_file_path);
        }
        g_free(smiley_file_path);
    }

    // Find the unicode and first ASCII version of smiley.
    string ascii_version;
    string unicode_version;
    for (size_t i = 1; i < v.size(); i++) {
        // Check if any chars are >= 128
        bool is_unicode = false;
        for (unsigned char c: v[i]) {
            if (c > 127) {
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
        return;
    }
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i] != unicode_version)
            ascii_to_unicode_smiley.insert(v[i].data(), unicode_version);
    }

    if (!ascii_version.empty()) {
        char* ascii_escaped = purple_markup_escape_text(ascii_version.data(), -1);
        unicode_to_ascii_smiley.insert(unicode_version.data(), ascii_escaped);
        g_free(ascii_escaped);
    }
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

            process_theme_smiley_line(v, buf, theme_dir.data(), theme_path);
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
        size_t ascii_len;
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
        size_t unicode_len;
        string* ascii = unicode_to_ascii_smiley.match(message.data() + index, &unicode_len);
        if (!ascii) {
            index++;
            continue;
        }

        message.replace(index, unicode_len, *ascii);
        index += ascii->length();
    }
}


void add_custom_smileys(PurpleConversation* conv, const char* message)
{
    char* unescaped_message = purple_unescape_text(message);
    for (const char* p = unescaped_message; *p != '\0';) {
        size_t smiley_length;
        shared_ptr<SmileyImage>* image_ptr = smiley_images.match(p, &smiley_length);
        if (image_ptr) {
            string smiley(p, p + smiley_length);
            SmileyImage& image = *(*image_ptr);

            // We use smileys as keys to check if we already set this custom smiley, otherwise
            // Pidgin will happily re-add smiley again and again.
            if (!purple_conversation_get_data(conv, smiley.data())
                    && purple_conv_custom_smiley_add(conv, smiley.data(), nullptr, nullptr, true)) {
                vkcom_debug_info("Adding custom smiley %s to conversation\n", smiley.data());
                purple_conversation_set_data(conv, smiley.data(), (void*)12345);
                purple_conv_custom_smiley_write(conv, smiley.data(),
                                                (const unsigned char*)image.data(),
                                                image.size());
                purple_conv_custom_smiley_close(conv, smiley.data());
            }
            p += smiley_length;
        } else {
            p++;
        }
    }
    g_free(unescaped_message);
}
