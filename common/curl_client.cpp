// curl_client.cpp - libcurl 实现
#include "curl_client.h"
#include <curl/curl.h>
#include <string>

namespace wddisk {

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    ((std::string*)userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

CurlClient::~CurlClient() {}

int CurlClient::request(const std::string& method, const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& body, std::string& response, std::string& errMsg) {

    CURL* curl = curl_easy_init();
    if (!curl) { errMsg = "curl init failed"; return -1; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    if (!cfg_.verifyTls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    struct curl_slist* hl = nullptr;
    std::string auth;
    if (!cfg_.username.empty() || !cfg_.password.empty()) {
        auth = cfg_.username + ":" + cfg_.password;
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }
    for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
    if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode rc = curl_easy_perform(curl);
    long code = -1;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    else errMsg = std::string("网络错误: ") + curl_easy_strerror(rc);

    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return (int)code;
}

} // namespace wddisk
