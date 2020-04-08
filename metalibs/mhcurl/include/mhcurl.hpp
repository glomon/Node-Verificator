#include <utility>

#ifndef MHCURL_HPP
#define MHCURL_HPP

#include <curl/curl.h>
#include <string>

class CurlFetch {
private:
    CURL* curl = nullptr;
    std::string host;
    int port;

public:
    CurlFetch(std::string _host, int _port)
        : host(std::move(_host))
        , port(_port)
    {
    }

    bool post(const std::string& url, const std::string& reques_string, std::string& response);

    bool post_singned(
        const std::string& url,
        const std::string& reques_string,
        const std::string& sign,
        const std::string& bubk,
        std::string& response);
};

#endif // MHCURL_HPP
