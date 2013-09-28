#include <cstring>
#include <debug.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

#include "httputils.h"
#include "miscutils.h"

#include "vk-auth.h"

namespace
{

// Collects all information required about a form in html page
struct HtmlForm
{
    string action_url;
    string method;
    string_map params; // Mapping from param name to param value if one exists
};

// Finds <form> element in the document
xmlNode* find_form_element(xmlDoc* doc);
// Finds HTML form (it must be the only one) in the document and return its parameters.
HtmlForm find_html_form(xmlDoc* doc);
// Returns POST headers for given url and data to be sent in x-www-form-urlencoded
PurpleHttpRequest* prepare_form_request(const HtmlForm& form);

xmlNode* find_form_element(xmlDoc* doc)
{
    xmlXPathContext* context = xmlXPathNewContext(doc);
    xmlXPathObject* result = xmlXPathEvalExpression((xmlChar*)"//form", context);
    xmlXPathFreeContext(context);
    xmlNodeSet* node_set = result->nodesetval;

    if (xmlXPathNodeSetGetLength(node_set) != 1) {
        purple_debug_error("prpl-vkcom", "Wrong number of <form>s in given html: %d\n",
                           xmlXPathNodeSetGetLength(node_set));
        return nullptr;
    }

    return node_set->nodeTab[0];
}

HtmlForm find_html_form(xmlDoc* doc)
{
    HtmlForm ret;

    xmlNode* form = find_form_element(doc);
    if (!form)
        return ret;

    ret.action_url = get_xml_node_prop(form, "action");
    ret.method = get_xml_node_prop(form, "method", "get");
    to_upper(ret.method);

    xmlXPathContext* context = xmlXPathNewContext(doc);
    context->node = form;
    xmlXPathObject* result = xmlXPathEvalExpression((xmlChar*)"//input", context);
    xmlXPathFreeContext(context);
    xmlNodeSet* node_set = result->nodesetval;

    for (int i = 0; i < node_set->nodeNr; i++) {
        xmlNode* input = node_set->nodeTab[i];
        string type = get_xml_node_prop(input, "type");
        if (type != "hidden" && type != "text" && type != "password")
            continue;
        string name = get_xml_node_prop(input, "name");
        string value = get_xml_node_prop(input, "value");
        ret.params[name] = value;
    }

    return ret;
}

PurpleHttpRequest* prepare_form_request(const HtmlForm& form)
{
    PurpleHttpRequest* req = purple_http_request_new(form.action_url.data());

    purple_http_request_set_method(req, form.method.data());
    purple_http_request_header_add(req, "Content-type", "application/x-www-form-urlencoded");

    string data = urlencode_form(form.params);
    purple_http_request_set_contents(req, data.data(), -1);

    return req;
}

// Class, containing all information, regarding authentication.
class VkAuthenticator
{
public:
    // Creates new VkAuthenticator object. The object MUST be allocated on heap, hence the private constructor.
    static VkAuthenticator* create(PurpleConnection* gc, const string& email, const string& password,
                                   const string& client_id, const string& scope, const AuthSuccessCb& success_cb,
                                   const ErrorCb& error_cb)
    {
        return new VkAuthenticator(gc, email, password, client_id, scope, success_cb, error_cb);
    }

    // Runs authentication process, ending up in calling either success_cb or error_cb
    void run();

private:
    PurpleConnection* m_gc;
    string m_email;
    string m_password;
    string m_client_id;
    string m_scope;

    AuthSuccessCb m_success_cb;
    ErrorCb m_error_cb;

    VkAuthenticator(PurpleConnection* gc, const string& email, const string& password, const string& client_id,
                    const string& scope, const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
        : m_gc(gc),
          m_email(email),
          m_password(password),
          m_client_id(client_id),
          m_scope(scope),
          m_success_cb(success_cb),
          m_error_cb(error_cb)
    {
    }

    ~VkAuthenticator()
    {
    }

    // Called upon success, destroys this.
    void on_success(const string& access_token, const string& uid);
    // Called upon failure, destroys this.
    void on_error(PurpleConnectionError error, const string& error_string);

    // First part of auth process: retrieves login page, finds relevant form with username (email) and password
    // and submits it.
    void on_fetch_vk_oauth_form(PurpleHttpConnection* http_conn, PurpleHttpResponse* response);
    // Second part of auth process: retrieves "confirm access" page and submits form. This part may be skipped.
    void on_fetch_vk_confirmation_form(PurpleHttpConnection* http_conn, PurpleHttpResponse* response);
    // Last part of auth process: retrieves access token. We either arrive here upon success from confirmation
    // page or upon error (when url starts with "http://oauth.vk.com/blank.html").
    void on_fetch_vk_access_token(PurpleHttpConnection* http_conn, PurpleHttpResponse*);
};


void VkAuthenticator::run()
{
    assert(m_success_cb);
    purple_connection_update_progress(m_gc, "Connecting", 0, 4);
    purple_debug_info("prpl-vkcom", "Starting authentication\n");

    string url = str_format("http://oauth.vk.com/oauth/authorize?redirect_uri=http://oauth.vk.com/blank.html"
                            "&response_type=token&client_id=%s&scope=%s&display=page", m_client_id.data(),
                            m_scope.data());
    http_get(m_gc, url, [this](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        on_fetch_vk_oauth_form(http_conn, response);
    });
}

void VkAuthenticator::on_success(const string& access_token, const string& uid)
{
    m_success_cb(access_token, uid);
    m_success_cb = nullptr;
    m_error_cb = nullptr;
    delete this;
}

void VkAuthenticator::on_error(PurpleConnectionError error, const string& error_string)
{
    purple_connection_error_reason(m_gc, error, error_string.data());
    if (m_error_cb)
        m_error_cb();
    m_success_cb = nullptr;
    m_error_cb = nullptr;
    delete this;
}

void VkAuthenticator::on_fetch_vk_oauth_form(PurpleHttpConnection* http_conn, PurpleHttpResponse* response)
{
    purple_connection_update_progress(m_gc, "Connecting", 1, 4);
    purple_debug_info("prpl-vkcom", "Fetched login page\n");

    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error retrieving login page: %s\n", purple_http_response_get_error(response));
        on_error(PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Error retrieving login page");
        return;
    }

    const char* page_data = purple_http_response_get_data(response, nullptr);
    xmlDoc* doc = htmlReadDoc((xmlChar*)page_data, nullptr, "utf-8", HTML_PARSE_RECOVER | HTML_PARSE_NOBLANKS
                              | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) {
        purple_debug_error("prpl-vkcom", "Unable to parse login form HTML: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    HtmlForm form = find_html_form(doc);
    xmlFreeDoc(doc);

    if (form.action_url.empty()) {
        purple_debug_error("prpl-vkcom", "Error finding form in login page: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    if (!map_update(form.params, "email", m_email)) {
        purple_debug_error("prpl-vkcom", "Login form does not contain email: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    if (!map_update(form.params, "pass", m_password)) {
        purple_debug_error("prpl-vkcom", "Login form does not contain pass: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    PurpleHttpRequest* request = prepare_form_request(form);
    http_request_copy_cookie_jar(request, http_conn);
    http_request_update_on_redirect(m_gc, request, [this](PurpleHttpConnection* new_conn, PurpleHttpResponse* new_response) {
        on_fetch_vk_confirmation_form(new_conn, new_response);
    });
    purple_http_request_unref(request);
}

void VkAuthenticator::on_fetch_vk_confirmation_form(PurpleHttpConnection* http_conn, PurpleHttpResponse* response)
{
    purple_connection_update_progress(m_gc, "Connecting", 2, 4);

    // Check if url contains "http://oauth.vk.com/blank.html"
    const char* url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    // Check if we must skip the confirmation form and get access token straight.
    if (g_str_has_prefix(url, "http://oauth.vk.com/blank.html")) {
        on_fetch_vk_access_token(http_conn, response);
        return;
    }

    purple_debug_info("prpl-vkcom", "Fetched login confirmation page");
    if (!purple_http_response_is_successful(response)) {
        purple_debug_error("prpl-vkcom", "Error retrieving login confirmation page: %s\n",
                           purple_http_response_get_error(response));
        on_error(PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Error retrieving login confirmation page");
        return;
    }

    const char* page_data = purple_http_response_get_data(response, nullptr);
    xmlDoc* doc = htmlReadDoc((xmlChar*)page_data, nullptr, "utf-8", HTML_PARSE_RECOVER | HTML_PARSE_NOBLANKS
                              | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) {
        purple_debug_error("prpl-vkcom", "Unable to parse confirmation form HTML: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }
    HtmlForm form = find_html_form(doc);
    xmlFreeDoc(doc);

    if (form.action_url.empty()) {
        purple_debug_error("prpl-vkcom", "Error finding form in login confirmation page: %s\n", page_data);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    }

    PurpleHttpRequest* request = prepare_form_request(form);
    http_request_copy_cookie_jar(request, http_conn);
    http_request_update_on_redirect(m_gc, request, [this](PurpleHttpConnection* new_conn, PurpleHttpResponse* new_response) {
        on_fetch_vk_access_token(new_conn, new_response);
    });
    purple_http_request_unref(request);
}

// Last part of auth process: retrieves access token. We either arrive here upon success from confirmation
// page or upon error (when url starts with "http://oauth.vk.com/blank.html").
void VkAuthenticator::on_fetch_vk_access_token(PurpleHttpConnection* http_conn, PurpleHttpResponse*)
{
    purple_connection_update_progress(m_gc, "Connecting", 3, 4);
    purple_debug_info("prpl-vkcom", "Fetched access token URL\n");

    // Check if url contains "http://oauth.vk.com/blank.html"
    const char* url = purple_http_request_get_url(purple_http_conn_get_request(http_conn));
    if (!g_str_has_prefix(url, "http://oauth.vk.com/blank.html")) {
        purple_debug_info("prpl-vkcom", "Error while getting access token: ended up with url %s\n", url);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, "Wrong username or password");
        return;
    }

    const char* url_params = strchr(url, '#') + 1;
    string_map params = parse_urlencoded_form(url_params);
    string access_token = params["access_token"];
    if (access_token.empty()) {
        purple_debug_error("prpl-vkcom", "access_token not present in %s\n", url_params);
        on_error(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE, "Internal auth error");
        return;
    } else {
        purple_connection_set_state(m_gc, PURPLE_CONNECTED);
        on_success(params["access_token"], params["user_id"]);
    }
}

} // End of anonymous namespace

void vk_auth_user(PurpleConnection* gc, const string& email, const string& password, const string& client_id, const string& scope,
                  const AuthSuccessCb& success_cb, const ErrorCb& error_cb)
{
    VkAuthenticator* authenticator = VkAuthenticator::create(gc, email, password, client_id, scope, success_cb, error_cb);
    authenticator->run();
}
