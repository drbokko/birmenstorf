/*
 * eiger_session.hpp — C++ wrapper around libeiger_client (host/port + typed config helpers).
 *
 * setDetectorConfig / setStreamConfig / setFilewriterConfig accept bool, int, double, or
 * const char* (JSON string value). The class builds the {"value": ...} fragment expected
 * by eiger_set_*_config so call sites do not hand-escape quotes.
 */

#ifndef EIGER_SESSION_HPP
#define EIGER_SESSION_HPP

#include "eiger_client.h"
#include <cstdio>
#include <cstddef>
#include <string>

namespace eiger_detail {

inline std::string json_value_fragment(bool v) { return v ? "true" : "false"; }

inline std::string json_value_fragment(int v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%d", v);
    return b;
}

inline std::string json_value_fragment(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.9f", v);
    return b;
}

// JSON string (quotes + minimal escaping) for enum-like values: "normal", "disabled", …
inline std::string json_value_fragment_string(const char *s) {
    std::string out;
    out.push_back('"');
    for (; s && *s; ++s) {
        if (*s == '\\' || *s == '"')
            out.push_back('\\');
        out.push_back(static_cast<char>(*s));
    }
    out.push_back('"');
    return out;
}

} // namespace eiger_detail

class EigerSession {
public:
    EigerSession(const char *host, int port) : host_(host), port_(port) {}

    int getStatus(const char *path, char *response, std::size_t response_size) const {
        return eiger_get_status(host_, port_, path, response, response_size);
    }

    int sendCommand(const char *command) const {
        return eiger_send_command(host_, port_, command);
    }

    int setDetectorConfig(const char *param, bool v) const {
        return eiger_set_detector_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setDetectorConfig(const char *param, int v) const {
        return eiger_set_detector_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setDetectorConfig(const char *param, double v) const {
        return eiger_set_detector_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    /** String parameter value (e.g. counting_mode "normal", threshold mode "enabled"). */
    int setDetectorConfig(const char *param, const char *string_value) const {
        return eiger_set_detector_config(host_, port_, param,
                                         eiger_detail::json_value_fragment_string(string_value).c_str());
    }

    int setStreamConfig(const char *param, bool v) const {
        return eiger_set_stream_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setStreamConfig(const char *param, int v) const {
        return eiger_set_stream_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setStreamConfig(const char *param, double v) const {
        return eiger_set_stream_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setStreamConfig(const char *param, const char *string_value) const {
        return eiger_set_stream_config(host_, port_, param,
                                       eiger_detail::json_value_fragment_string(string_value).c_str());
    }

    int setMonitorConfig(const char *param, const char *value) const {
        return eiger_set_monitor_config(host_, port_, param, value);
    }

    int setFilewriterConfig(const char *param, bool v) const {
        return eiger_set_filewriter_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setFilewriterConfig(const char *param, int v) const {
        return eiger_set_filewriter_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setFilewriterConfig(const char *param, double v) const {
        return eiger_set_filewriter_config(host_, port_, param, eiger_detail::json_value_fragment(v).c_str());
    }
    int setFilewriterConfig(const char *param, const char *string_value) const {
        return eiger_set_filewriter_config(host_, port_, param,
                                           eiger_detail::json_value_fragment_string(string_value).c_str());
    }

    const char *host() const { return host_; }
    int port() const { return port_; }

private:
    const char *host_;
    int port_;
};

#endif
