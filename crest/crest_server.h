#ifndef CREST_SERVER_H
#define CREST_SERVER_H

#include <string>
#include <unordered_map>
#include <functional>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <sys/un.h> // For UNIX domain sockets
#include "spdlog/spdlog.h"

enum LogLevel
{
    LOG_ERROR = 0,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG
};

struct ServerConfig
{
    std::string ip;
    int port;
    LogLevel logLevel;
    int nthreads;

    ServerConfig(const std::string &ip = "127.0.0.1", int port = 2424,
                 LogLevel logLevel = LOG_INFO, int nthreads = 4) : ip(ip),
                                                                   port(port), logLevel(logLevel), nthreads(nthreads) {}
};

struct WorkerThread
{
    pthread_t thread_id;
    struct event_base *base;
    struct event *notify_event;
    int notify_receive_fd;
    int notify_send_fd;
    std::queue<int> conn_queue;
    pthread_mutex_t queue_lock;
};

class Server
{
public:
    Server(const ServerConfig &config);
    ~Server();

    void init();
    void run();

private:
    ServerConfig config;
    struct event_base *base;
    struct evconnlistener *listener;
    struct event *signal_event;
    struct event *sigterm_event;

    static void listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                            struct sockaddr *sa, int socklen, void *user_data);
    static void conn_readcb(struct bufferevent *bev, void *user_data);
    static void conn_eventcb(struct bufferevent *bev, short events, void *user_data);
    static void signal_cb(evutil_socket_t sig, short events, void *user_data);

    void processCommand(struct bufferevent *bev, const std::string &command);
    bool setupListeningSocket();

    // Thread management
    static void *worker_thread_main(void *arg);
    void dispatch_conn(int fd);

    std::vector<WorkerThread> worker_threads;
    size_t next_worker;
};

#endif // CREST_SERVER_H