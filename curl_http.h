#pragma once
#include <string>

enum class HttpStatus
{
    Pending,
    Completed,
    Failed
};

struct HttpRequest
{
    int status_code;
    const char* response_data;
    size_t response_data_len;
};

HttpRequest* http_get(const char* url);
HttpStatus http_process(HttpRequest* request);
void http_release(HttpRequest* request);
