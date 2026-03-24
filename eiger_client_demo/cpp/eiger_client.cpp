/*
 * eiger_client.cpp - EIGER detector REST API client implementation (C++ source, C ABI).
 */

#include "eiger_client.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#if !defined(_WIN32) && defined(USE_LIBCURL) && (USE_LIBCURL)
#include <curl/curl.h>
#endif

namespace {

#if !defined(_WIN32) && defined(USE_LIBCURL) && (USE_LIBCURL)
struct EigerCurlMem {
    char *memory;
    size_t size;
    size_t capacity;
};

size_t eiger_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *mem = static_cast<EigerCurlMem *>(userp);
    if (mem->size + realsize > mem->capacity)
        realsize = mem->capacity - mem->size;
    if (realsize) {
        std::memcpy(mem->memory + mem->size, contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = '\0';
    }
    return size * nmemb;
}
#endif

int build_value_body(char *buf, size_t buf_size, const char *value_json) {
    int n = std::snprintf(buf, buf_size, "{\"value\": %s}", value_json);
    return (n >= 0 && static_cast<size_t>(n) < buf_size) ? 0 : -1;
}

int eiger_set_config(const char *host, int port, const char *api_prefix,
                     const char *param, const char *body) {
    char api_path[512];
    std::snprintf(api_path, sizeof(api_path), "/%s/api/%s/config/%s", api_prefix,
                  eiger_client_api_version(), param);
    return eiger_http_request(host, port, "PUT", api_path, body, nullptr, 0);
}

} // namespace

static FILE *g_eiger_http_trace;

extern "C" {

const char *eiger_client_api_version(void) {
    const char *e = std::getenv("EIGER_API_VERSION");
    return (e && e[0]) ? e : "1.8.0";
}

void eiger_set_http_trace(FILE *fp) {
    g_eiger_http_trace = fp;
}

int eiger_http_request(const char *host, int port,
                       const char *method, const char *path,
                       const char *data,
                       char *response, size_t response_size) {
    const char *send_body = nullptr;
    size_t send_len = 0;

    if (std::strcmp(method, "PUT") == 0) {
        if (data && data[0] != '\0') {
            send_body = data;
            send_len = std::strlen(data);
        } else {
            send_body = "{}";
            send_len = 2;
        }
    }

    if (g_eiger_http_trace) {
        std::fprintf(g_eiger_http_trace, "[eiger] %s http://%s:%d%s", method, host, port, path);
        if (std::strcmp(method, "PUT") == 0)
            std::fprintf(g_eiger_http_trace, "  body: %.*s", static_cast<int>(send_len), send_body);
        std::fprintf(g_eiger_http_trace, "\n");
        std::fflush(g_eiger_http_trace);
    }

#ifdef _WIN32
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    int result = -1;
    DWORD dwStatusCode = 0;
    DWORD dwStatusCodeSize = sizeof(dwStatusCode);

    hSession = WinHttpOpen(L"EigerClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return -1;

    WCHAR wHost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, static_cast<int>(sizeof(wHost) / sizeof(WCHAR)));

    hConnect = WinHttpConnect(hSession, wHost, static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return -1;
    }

    WCHAR wPath[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, static_cast<int>(sizeof(wPath) / sizeof(WCHAR)));

    hRequest = WinHttpOpenRequest(hConnect,
                                  (std::strcmp(method, "GET") == 0) ? L"GET" : L"PUT",
                                  wPath, nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (std::strcmp(method, "PUT") == 0 && send_body && send_len > 0) {
        /*
         * Entity body must be UTF-8 JSON bytes. Do NOT send UTF-16 (WCHAR):
         * converting the body with MultiByteToWideChar and passing those
         * bytes breaks the API (detector expects ASCII/UTF-8 JSON).
         */
        WCHAR wContentType[] = L"Content-Type: application/json; charset=utf-8\r\n";
        WinHttpAddRequestHeaders(hRequest, wContentType, -1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                reinterpret_cast<LPVOID>(const_cast<char *>(send_body)),
                                static_cast<DWORD>(send_len), static_cast<DWORD>(send_len), 0)) {
            std::fprintf(stderr, "eiger_client: WinHttpSendRequest failed %s %s (err=%lu)\n",
                         method, path, static_cast<unsigned long>(GetLastError()));
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return -1;
        }
    } else if (std::strcmp(method, "GET") == 0) {
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return -1;
        }
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwStatusCodeSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    if (dwStatusCode < 200 || dwStatusCode >= 300) {
        std::fprintf(stderr,
                     "eiger_client: HTTP %lu %s %s (check EIGER_API_VERSION, default is 1.8.0)\n",
                     static_cast<unsigned long>(dwStatusCode), method, path);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (response && response_size > 0) {
        response[0] = '\0';
        size_t offset = 0;
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0)
                break;
            if (offset + static_cast<size_t>(dwSize) >= response_size)
                dwSize = static_cast<DWORD>(response_size - offset - 1);
            if (dwSize == 0)
                break;
            if (!WinHttpReadData(hRequest, response + offset, dwSize, &dwDownloaded))
                break;
            offset += static_cast<size_t>(dwDownloaded);
            response[offset] = '\0';
        } while (dwSize > 0);
    }

    result = 0;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;

#else
#if defined(USE_LIBCURL) && (USE_LIBCURL)
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    char url[1024];
    std::snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    if (std::strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, send_body);
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json; charset=utf-8");
    if (std::strcmp(method, "PUT") == 0)
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (response && response_size > 0) {
        response[0] = '\0';
        EigerCurlMem chunk{};
        chunk.memory = response;
        chunk.size = 0;
        chunk.capacity = response_size - 1;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, eiger_curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void *>(&chunk));
    }

    CURLcode res = curl_easy_perform(curl);
    if (headers)
        curl_slist_free_all(headers);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code < 200 || code >= 300) {
        std::fprintf(stderr,
                     "eiger_client: HTTP %ld %s %s (curl=%d, check EIGER_API_VERSION)\n",
                     code, method, path, static_cast<int>(res));
        return -1;
    }
    return 0;
#else
    (void)host;
    (void)port;
    (void)method;
    (void)path;
    (void)data;
    (void)send_body;
    (void)send_len;
    (void)response;
    (void)response_size;
    std::fprintf(stderr,
                 "eiger_client: HTTP not implemented on this platform (use Windows or build with "
                 "USE_LIBCURL and link libcurl)\n");
    return -1;
#endif
#endif
}

int eiger_get_status(const char *host, int port, const char *path,
                     char *response, size_t response_size) {
    char api_path[512];
    std::snprintf(api_path, sizeof(api_path), "/detector/api/%s/status/%s",
                  eiger_client_api_version(), path);
    return eiger_http_request(host, port, "GET", api_path, nullptr, response, response_size);
}

int eiger_send_command(const char *host, int port, const char *command) {
    char api_path[512];
    std::snprintf(api_path, sizeof(api_path), "/detector/api/%s/command/%s",
                  eiger_client_api_version(), command);
    return eiger_http_request(host, port, "PUT", api_path, "{}", nullptr, 0);
}

int eiger_set_detector_config(const char *host, int port,
                              const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0)
        return -1;
    return eiger_set_config(host, port, "detector", param, body);
}

int eiger_set_stream_config(const char *host, int port,
                            const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0)
        return -1;
    return eiger_set_config(host, port, "stream", param, body);
}

int eiger_set_monitor_config(const char *host, int port,
                             const char *param, const char *value) {
    char body[256];
    std::snprintf(body, sizeof(body), "{\"value\": \"%s\"}", value);
    return eiger_set_config(host, port, "monitor", param, body);
}

int eiger_set_filewriter_config(const char *host, int port,
                                const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0)
        return -1;
    return eiger_set_config(host, port, "filewriter", param, body);
}

} // extern "C"
