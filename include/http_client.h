#pragma once

#include <string>
#include <curl/curl.h>

/*
  NOTE: This is a really primitive blocking client meant only for specific Typesense use cases.
*/
class HttpClient {
private:
    std::string buffer;
    std::string url;
    std::string api_key;

public:
    HttpClient(std::string url, std::string api_key): url(url), api_key(api_key) {

    }

    static size_t curl_write (void *contents, size_t size, size_t nmemb, std::string *s) {
        s->append((char*)contents, size*nmemb);
        return size*nmemb;
    }

    long get_reponse(std::string & response) {
        CURL *curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // to allow self-signed certs
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpClient::curl_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

        struct curl_slist *chunk = NULL;
        std::string api_key_header = std::string("x-typesense-api-key: ") + api_key;
        chunk = curl_slist_append(chunk, api_key_header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        response = buffer;
        return http_code;
    }
};