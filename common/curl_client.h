// curl_client.h - macOS/Linux 通用 HTTP 实现（libcurl）
#pragma once
#include "../common/webdav.h"

namespace wddisk {

class CurlClient : public IHttpClient {
public:
    explicit CurlClient(const DavConfig& cfg) : cfg_(cfg) {}
    ~CurlClient() override;
    int request(const std::string& method, const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& body, std::string& response, std::string& errMsg) override;
private:
    DavConfig cfg_;
};

} // namespace wddisk
