#pragma once
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <sys/socket.h>

// Keep diagnostics off the successful-operation path and bound payload output.
inline void logExchangeFailure(const char *stage, int sockfd, int error,
                        const std::string &command, const std::string &response)
{
    auto hexPrefix = [](const std::string &text, size_t limit)
    {
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (size_t i = 0; i < std::min(text.size(), limit); ++i)
            hex << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(text[i]));
        return hex.str();
    };
    auto offset = [](size_t position) -> long long
    {
        return position == std::string::npos ? -1 : static_cast<long long>(position);
    };
    std::ostringstream message;
    message << "Exchange failure: stage=" << stage
            << " thread=" << std::this_thread::get_id() << " fd=" << sockfd
            << " errno=" << error << " (" << std::strerror(error) << ")"
            << " command_bytes=" << command.size()
            << " command_prefix_hex=" << hexPrefix(command, 128)
            << " response_bytes=" << response.size()
            << " first_lf=" << offset(response.find('\n'))
            << " first_nul=" << offset(response.find('\0'))
            << " response_prefix_hex=" << hexPrefix(response, 256) << '\n';
    static std::mutex diagnostic_mutex;
    std::lock_guard<std::mutex> lock(diagnostic_mutex);
    std::cerr << message.str();
}

// Crest uses one newline-terminated request and reply per operation.
inline bool exchangeCommand(int sockfd, const std::string &command, std::string &response)
{
    std::string request = command + "\n";
    size_t sent = 0;
    while (sent < request.size())
    {
        ssize_t n = send(sockfd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            int error = n < 0 ? errno : 0;
            logExchangeFailure(n < 0 ? "send_error" : "send_zero", sockfd, error, command, response);
            return false;
        }
        sent += n;
    }

    response.clear();
    char buffer[65536];
    while (response.empty() || response.back() != '\n')
    {
        ssize_t n = recv(sockfd, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            int error = n < 0 ? errno : 0;
            logExchangeFailure(n < 0 ? "recv_error" : "recv_eof", sockfd, error, command, response);
            return false;
        }
        response.append(buffer, n);
    }
    response.pop_back();
    if (!response.empty() && response.back() == '\r')
        response.pop_back();
    if (response.find('\n') != std::string::npos)
    {
        logExchangeFailure("embedded_newline", sockfd, 0, command, response);
        return false;
    }
    return true;
}
