#include <cstdlib>

#include <debug.h>

#include "miscutils.h"

#include "vk-auth.h"

#include "vk-common.h"

const char VK_CLIENT_ID[] = "3833170";

namespace {

// Splits the comma-separated string of integers.
uint64_set str_split_int(const char* str)
{
    uint64_set ret;
    while (*str) {
        char* next;
        uint64 i = strtoll(str, &next, 10);
        ret.insert(i);
        if (*next) {
            assert(*next == ',');
            str = next + 1;
        } else {
            str = next;
        }
    }
    return ret;
}

// Parses VkReceivedMessages from JSON representation.
vector<VkReceivedMessage> deferred_mark_as_read_from_string(const char* str)
{
    picojson::value v;
    string err = picojson::parse(v, str, str + strlen(str));
    if (!err.empty() || !v.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Error loading uploaded docs: %s\n", err.data());
        return {};
    }

    vector<VkReceivedMessage> ret;
    const picojson::array& a = v.get<picojson::array>();
    for (const picojson::value& d: a) {
        ret.push_back(VkReceivedMessage());
        VkReceivedMessage& msg = ret.back();
        msg.msg_id = d.get("msg_id").get<double>();
        msg.user_id = d.get("user_id").get<double>();
        msg.chat_id = d.get("chat_id").get<double>();
    }
    return ret;
}

// Stores VkReceivedMessages in JSON representation.
string deferred_mark_as_read_to_string(const vector<VkReceivedMessage>& messages)
{
    picojson::array a;
    for (const VkReceivedMessage& msg: messages) {
        picojson::object d = {
            {"msg_id",  picojson::value((double)msg.msg_id)},
            {"user_id", picojson::value((double)msg.user_id)},
            {"chat_id", picojson::value((double)msg.chat_id)},
        };
        a.push_back(picojson::value(d));
    }
    return picojson::value(a).serialize();
}

// Parses VkUploadedDocs from JSON representation.
vector<VkUploadedDoc> uploaded_docs_from_string(const char* str)
{
    picojson::value v;
    string err = picojson::parse(v, str, str + strlen(str));
    if (!err.empty() || !v.is<picojson::array>()) {
        purple_debug_error("prpl-vkcom", "Error loading uploaded docs: %s\n", err.data());
        return {};
    }

    vector<VkUploadedDoc> ret;
    const picojson::array& a = v.get<picojson::array>();
    for (const picojson::value& d: a) {
        // Compatibility with older releases.
        if (!field_is_present<double>(d, "id") || !field_is_present<string>(d, "filename")
                || !field_is_present<double>(d, "size") || !field_is_present<string>(d, "md5sum")
                || !field_is_present<string>(d, "url"))
            continue;

        ret.push_back(VkUploadedDoc());
        VkUploadedDoc& doc = ret.back();
        doc.id = d.get("id").get<double>();
        doc.filename = d.get("filename").get<string>();
        doc.size = d.get("size").get<double>();
        doc.md5sum = d.get("md5sum").get<string>();
        doc.url = d.get("url").get<string>();
    }
    return ret;
}

// Stores VkUploadedDocs in JSON representation.
string uploaded_docs_to_string(const vector<VkUploadedDoc>& docs)
{
    picojson::array a;
    for (const VkUploadedDoc& doc: docs) {
        picojson::object d = {
            {"id",  picojson::value((double)doc.id)},
            {"filename", picojson::value(doc.filename)},
            {"size", picojson::value((double)doc.size)},
            {"md5sum", picojson::value(doc.md5sum)},
            {"url", picojson::value(doc.url)}
        };
        a.push_back(picojson::value(d));
    }
    return picojson::value(a).serialize();
}

} // namespace

VkConnData::VkConnData(PurpleConnection* gc, const string& email, const string& password)
    : keepalive_pool(nullptr),
      m_email(email),
      m_password(password),
      m_gc(gc),
      m_closing(false)
{
    PurpleAccount* account = purple_connection_get_account(m_gc);
    const char* str = purple_account_get_string(account, "manually_added_buddies", "");
    manually_added_buddies = str_split_int(str);

    str = purple_account_get_string(account, "manually_removed_buddies", "");
    manually_removed_buddies = str_split_int(str);

    str = purple_account_get_string(account, "deferred_mark_as_read", "");
    deferred_mark_as_read = deferred_mark_as_read_from_string(str);

    str = purple_account_get_string(account, "uploaded_docs", "[]");
    uploaded_docs = uploaded_docs_from_string(str);
}

VkConnData::~VkConnData()
{
    PurpleAccount* account = purple_connection_get_account(m_gc);
    string str = str_concat_int(',', manually_added_buddies);
    purple_account_set_string(account, "manually_added_buddies", str.data());

    str = str_concat_int(',', manually_removed_buddies);
    purple_account_set_string(account, "manually_removed_buddies", str.data());

    str = deferred_mark_as_read_to_string(deferred_mark_as_read);
    purple_account_set_string(account, "deferred_mark_as_read", str.data());

    str = uploaded_docs_to_string(uploaded_docs);
    purple_account_set_string(account, "uploaded_docs", str.data());
}

void VkConnData::authenticate(const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    m_access_token.clear();
    vk_auth_user(m_gc, m_email, m_password, VK_CLIENT_ID, "friends,photos,audio,video,docs,messages",
        [=](const string& access_token, const string& uid) {
            m_access_token = access_token;
            try {
                m_uid = atoll(uid.data());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-fpermissive" // catch (...) makes GCC 4.7.2 return strange error, fixed in later GCCs
#endif
            } catch (...) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
                purple_debug_error("prpl-vkcom", "Error converting uid %s to integer\n", uid.data());
                purple_connection_error_reason(m_gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Authentication process failed");
                error_cb();
            }
            success_cb();
    }, [=] {
        purple_debug_error("prpl-vkcom", "Unable authenticate, connection will be terminated\n");
        purple_connection_error_reason(m_gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Unable to connect to Long Poll server");
        error_cb();
    });
}


string buddy_name_from_uid(uint64 uid)
{
    return str_format("id%" PRIu64, uid);
}

uint64 uid_from_buddy_name(const char* name)
{
    if (strncmp(name, "id", 2) != 0)
        return 0;
    return atoll(name + 2);
}

string chat_name_from_id(uint64 chat_id)
{
    return str_format("chat%" PRIu64, chat_id);
}

uint64 chat_id_from_name(const char* name)
{
    if (strncmp(name, "chat", 4) != 0)
        return 0;
    return atoll(name + 4);
}
