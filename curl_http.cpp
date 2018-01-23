#include "curl_http.h"

#define CURL_STATICLIB
#pragma comment(lib, "libcurl_a.lib")
#include "curl/curl.h"

#include <mutex>

struct HttpRequestData
{
    HttpRequest request;
    char error_buffer[CURLOPT_ERRORBUFFER];
    std::string buffer;
    std::mutex lock;
    HttpStatus status;
};

bool gIsInit = false;

static int OnHttpData(char* data, size_t size, size_t nmemb, HttpRequestData* request_data)
{
    size_t sizeBytes = size*nmemb;
    if (request_data)
        request_data->buffer.append(data, sizeBytes);
    return sizeBytes;
}

HttpRequest* http_get(const char* url)
{
    if (!gIsInit) {
        gIsInit = true;
        curl_global_init(CURL_GLOBAL_ALL);
    }

    HttpRequestData* data = new HttpRequestData();
    data->status = HttpStatus::Pending;
    data->buffer.clear();
    data->error_buffer[0] = '\0';
    data->request.response_data_len = 0;
    data->request.status_code = 0;

    std::thread([data, url]()
    {
        CURL* curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnHttpData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, data->error_buffer);
        CURLcode result = curl_easy_perform(curl);
        {
            std::lock_guard<std::mutex> lock(data->lock);
            if (result == CURLE_OK) {
                data->status = HttpStatus::Completed;
                data->request.response_data = data->buffer.c_str();
                data->request.response_data_len = data->buffer.size();
            }
            else {
                data->status = HttpStatus::Failed;
                data->request.status_code = result;
                data->request.response_data = data->error_buffer;
                data->request.response_data_len = strlen(data->error_buffer);
            }
        }
        curl_easy_cleanup(curl);
    }).detach();

    return &data->request;
}

HttpStatus http_process(HttpRequest* request)
{
    HttpRequestData* data = ((HttpRequestData*)request);
    std::lock_guard<std::mutex> lock(data->lock);
    return data->status;
}

void http_release(HttpRequest* request)
{
    delete ((HttpRequestData*)request);
}
