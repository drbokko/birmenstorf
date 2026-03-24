/*
 * eiger_session.hpp — Thin C++ wrapper around libeiger_client (stores host/port).
 */

#ifndef EIGER_SESSION_HPP
#define EIGER_SESSION_HPP

#include "eiger_client.h"
#include <cstddef>

class EigerSession {
public:
    EigerSession(const char *host, int port) : host_(host), port_(port) {}

    int getStatus(const char *path, char *response, std::size_t response_size) const {
        return eiger_get_status(host_, port_, path, response, response_size);
    }

    int sendCommand(const char *command) const {
        return eiger_send_command(host_, port_, command);
    }

    int setDetectorConfig(const char *param, const char *value_json) const {
        return eiger_set_detector_config(host_, port_, param, value_json);
    }

    int setStreamConfig(const char *param, const char *value_json) const {
        return eiger_set_stream_config(host_, port_, param, value_json);
    }

    int setMonitorConfig(const char *param, const char *value) const {
        return eiger_set_monitor_config(host_, port_, param, value);
    }

    int setFilewriterConfig(const char *param, const char *value_json) const {
        return eiger_set_filewriter_config(host_, port_, param, value_json);
    }

    const char *host() const { return host_; }
    int port() const { return port_; }

private:
    const char *host_;
    int port_;
};

#endif
