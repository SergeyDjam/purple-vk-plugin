#include <cstdlib>

#include <debug.h>

#include "strutils.h"

#include "miscutils.h"

#include "vk-auth.h"
#include "vk-common.h"

const char VK_CLIENT_ID[] = "3833170";
const char VK_PERMISSIONS[] = "friends,photos,audio,video,docs,messages,offline";

namespace {

// Splits the comma-separated string of integers.
set<uint64> str_split_int(const char* str)
{
    set<uint64> ret;
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
    vector<VkReceivedMessage> messages;

    picojson::value v;
    string err = picojson::parse(v, str, str + strlen(str));
    if (!err.empty() || !v.is<picojson::array>()) {
        vkcom_debug_error("Error loading uploaded docs: %s\n", err.data());
        return messages;
    }

    const picojson::array& a = v.get<picojson::array>();
    for (const picojson::value& d: a) {
        VkReceivedMessage msg;
        msg.msg_id = d.get("msg_id").get<double>();
        msg.user_id = d.get("user_id").get<double>();
        msg.chat_id = d.get("chat_id").get<double>();
        messages.push_back(std::move(msg));
    }

    vkcom_debug_info("%d messages marked as unread\n", (int)messages.size());

    return messages;
}

// Stores VkReceivedMessages in JSON representation.
string deferred_mark_as_read_to_string(const vector<VkReceivedMessage>& messages)
{
    vkcom_debug_info("%d messages still marked as unread\n", (int)messages.size());

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
map<uint64, VkUploadedDocInfo> uploaded_docs_from_string(const char* str)
{
    map<uint64, VkUploadedDocInfo> docs;

    picojson::value v;
    string err = picojson::parse(v, str, str + strlen(str));
    if (!err.empty() || !v.is<picojson::array>()) {
        vkcom_debug_error("Error loading uploaded docs: %s\n", err.data());
        return docs;
    }

    const picojson::array& a = v.get<picojson::array>();
    for (const picojson::value& d: a) {
        // Compatibility with older releases.
        if (!field_is_present<double>(d, "id") || !field_is_present<string>(d, "filename")
                || !field_is_present<double>(d, "size") || !field_is_present<string>(d, "md5sum")
                || !field_is_present<string>(d, "url"))
            continue;

        uint64 id = d.get("id").get<double>();
        VkUploadedDocInfo& doc = docs[id];
        doc.filename = d.get("filename").get<string>();
        doc.size = d.get("size").get<double>();
        doc.md5sum = d.get("md5sum").get<string>();
        doc.url = d.get("url").get<string>();
    }
    return docs;
}

// Stores VkUploadedDocs in JSON representation.
string uploaded_docs_to_string(const map<uint64, VkUploadedDocInfo>& docs)
{
    picojson::array a;
    for (const pair<uint64, VkUploadedDocInfo>& p: docs) {
        uint64 id = p.first;
        const VkUploadedDocInfo& doc = p.second;
        picojson::object d = {
            {"id",  picojson::value((double)id)},
            {"filename", picojson::value(doc.filename)},
            {"size", picojson::value((double)doc.size)},
            {"md5sum", picojson::value(doc.md5sum)},
            {"url", picojson::value(doc.url)}
        };
        a.push_back(picojson::value(d));
    }
    return picojson::value(a).serialize();
}

} // End of anonymous namespace

VkData::VkData(PurpleConnection* gc, const string& email, const string& password)
    : m_email(email),
      m_password(password),
      m_gc(gc),
      m_closing(false),
      m_keepalive_pool(nullptr)
{
    PurpleAccount* account = purple_connection_get_account(m_gc);

    m_access_token = purple_account_get_string(account, "access_token", "");
    // Ids are 64-bit integers, so let's store this id in a string representation.
    m_self_user_id = atoll(purple_account_get_string(account, "self_user_id", "0"));

    m_options.only_friends_in_blist = purple_account_get_bool(account, "only_friends_in_blist", false);
    m_options.chats_in_blist = purple_account_get_bool(account, "chats_in_blist", true);
    m_options.mark_as_read_online_only = purple_account_get_bool(account, "mark_as_read_online_only", true);
    m_options.imitate_mobile_client = purple_account_get_bool(account, "imitate_mobile_client", false);
    m_options.blist_default_group = purple_account_get_string(account, "blist_default_group", "");
    m_options.blist_chat_group = purple_account_get_string(account, "blist_chat_group", "");

    const char* str = purple_account_get_string(account, "manually_added_buddies", "");
    m_manually_added_buddies = str_split_int(str);

    str = purple_account_get_string(account, "manually_removed_buddies", "");
    m_manually_removed_buddies = str_split_int(str);

    str = purple_account_get_string(account, "manually_added_chats", "");
    m_manually_added_chats = str_split_int(str);

    str = purple_account_get_string(account, "manually_removed_chats", "");
    m_manually_removed_chats = str_split_int(str);

    str = purple_account_get_string(account, "deferred_mark_as_read", "");
    deferred_mark_as_read = deferred_mark_as_read_from_string(str);

    str = purple_account_get_string(account, "uploaded_docs", "[]");
    uploaded_docs = uploaded_docs_from_string(str);
}

VkData::~VkData()
{
    PurpleAccount* account = purple_connection_get_account(m_gc);

    purple_account_set_string(account, "access_token", m_access_token.data());
    purple_account_set_string(account, "self_user_id", to_string(m_self_user_id).data());

    string str = str_concat_int(',', m_manually_added_buddies);
    purple_account_set_string(account, "manually_added_buddies", str.data());

    str = str_concat_int(',', m_manually_removed_buddies);
    purple_account_set_string(account, "manually_removed_buddies", str.data());

    str = str_concat_int(',', m_manually_added_chats);
    purple_account_set_string(account, "manually_added_chats", str.data());

    str = str_concat_int(',', m_manually_removed_chats);
    purple_account_set_string(account, "manually_removed_chats", str.data());

    str = deferred_mark_as_read_to_string(deferred_mark_as_read);
    purple_account_set_string(account, "deferred_mark_as_read", str.data());

    str = uploaded_docs_to_string(uploaded_docs);
    purple_account_set_string(account, "uploaded_docs", str.data());

    // g_source_remove calls timeout_destroy_cb, which modifies timeout_ids, so we make a copy before
    // calling g_source_remove. Damned mutability.
    set<unsigned> timeout_ids_copy = timeout_ids;
    for (unsigned id: timeout_ids_copy)
        g_source_remove(id);

    if (m_keepalive_pool)
        purple_http_keepalive_pool_unref(m_keepalive_pool);
}

void VkData::authenticate(const SuccessCb& success_cb, const ErrorCb& error_cb)
{
    if (!m_access_token.empty()) {
        vkcom_debug_info("No need to auth, we have an access token\n");
        purple_connection_set_state(m_gc, PURPLE_CONNECTED);
        success_cb();
        return;
    }

    vk_auth_user(m_gc, m_email, m_password, VK_CLIENT_ID, VK_PERMISSIONS,
                 m_options.imitate_mobile_client,
        [=](const string& access_token, const string& self_user_id) {
            m_access_token = access_token;
            try {
                m_self_user_id = atoll(self_user_id.data());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
// catch (...) makes GCC 4.7.2 return strange error, fixed in later GCCs
#pragma GCC diagnostic ignored "-fpermissive"
#endif
            } catch (...) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
                vkcom_debug_error("Error converting user id %s to integer\n", self_user_id.data());
                purple_connection_error_reason(m_gc, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                                               i18n("Authentication process failed"));
                error_cb();
            }
            success_cb();
    }, [=] {
        vkcom_debug_error("Unable to authenticate, connection will be terminated\n");
        purple_connection_error_reason(m_gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                                       i18n("Unable to connect to Long Poll server"));
        error_cb();
    });
}

PurpleHttpKeepalivePool* VkData::get_keepalive_pool()
{
    if (!m_keepalive_pool)
        m_keepalive_pool = purple_http_keepalive_pool_new();

    return m_keepalive_pool;
}


string user_name_from_id(uint64 user_id)
{
    return str_format("id%" PRIu64, user_id);
}

uint64 user_id_from_name(const char* name, bool quiet)
{
    if (strncmp(name, "id", 2) != 0) {
        if (!quiet)
            vkcom_debug_error("Unknown username %s\n", name);
        return 0;
    }

    return atoll(name + 2);
}

// NOTE: Pidgin employs different schemes of identifying multiuser chats (unlike users where username is the one
// and only identifier for all operations):
//   1) chat components --- a hash table string -> string, which is stored in blist;
//   2) chat name --- a string, which is not stored in blist, but gets computed on the fly (see vk_get_chat_nam
//      and vk_find_blist_chat);
//   3) open chat conversation id --- an integer.
//
// chat_name_from_id is used as chat name (2) and gets stored in components (1) by the key "id". The open chat
// conversation id is generated when opening chat conversation window.
string chat_name_from_id(uint64 chat_id)
{
    return str_format("chat%" PRIu64, chat_id);
}

uint64 chat_id_from_name(const char* name, bool quiet)
{
    if (strncmp(name, "chat", 4) != 0) {
        if (!quiet)
            vkcom_debug_error("Unknown chatname %s\n", name);
        return 0;
    }

    return atoll(name + 4);
}


void timeout_add(PurpleConnection* gc, unsigned milliseconds, const TimeoutCb& callback)
{
    // Helper structure. The two latter members are used to remove id upon timeout end.
    struct TimeoutCbData
    {
        TimeoutCb callback;
        VkData& gc_data;
        unsigned id;
    };

    VkData& gc_data = get_data(gc);
    if (gc_data.is_closing()) {
        vkcom_debug_error("Programming error: timeout_add(%d) called during logout\n", milliseconds);
        return;
    }

    TimeoutCbData* data = new TimeoutCbData({ callback, gc_data, 0 });
    data->id = g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, [](void* user_data) -> gboolean {
        TimeoutCbData* param = (TimeoutCbData*)user_data;
        return param->callback();
    }, data, [](void* user_data) {
        TimeoutCbData* param = (TimeoutCbData*)user_data;
        param->gc_data.timeout_ids.erase(param->id);
        delete param;
    });

    gc_data.timeout_ids.insert(data->id);
}
