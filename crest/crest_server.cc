#include "crest_server.h"
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h> // For UNIX domain sockets
#include <vector>
#include <queue>
#include "ProcessCommand.h"
#include <malloc.h>
#include <stdlib.h>

// New Connection → listener_cb → dispatch_conn → Worker Thread → Connection Processing

/*
 * Constructed in main(), NOT statically: the dict constructor may create
 * guides (e.g., skip-list sentinel nodes), and static initialization order
 * across translation units is unspecified -- this TU's globals initialize
 * before globalConfig.cc's SODA bitmap and global_addr_start, so guides
 * created here would silently never register in SODA. An unregistered but
 * frequently dereferenced guide leaks ATC increments (decrements are
 * skipped when the SODA bit is clear) until the ATC field saturates.
 */
static ProcessCommand *processor = nullptr;

Server::Server(const ServerConfig &config) : config(config), base(nullptr), listener(nullptr), signal_event(nullptr), sigterm_event(nullptr), next_worker(0)
{
    spdlog::set_level(static_cast<spdlog::level::level_enum>(config.logLevel));
    spdlog::info("Server initialized with IP: {}, Port: {}, LogLevel: {}", config.ip, config.port, config.logLevel);
}

Server::~Server()
{
    if (listener)
    {
        evconnlistener_free(listener);
    }
    if (signal_event)
    {
        event_free(signal_event);
    }
    if (sigterm_event)
    {
        event_free(sigterm_event);
    }
    if (base)
    {
        event_base_free(base);
    }
    for (auto &worker : worker_threads)
    {
        if (worker.notify_event)
        {
            event_free(worker.notify_event);
        }
        if (worker.base)
        {
            event_base_free(worker.base);
        }
        close(worker.notify_receive_fd);
        close(worker.notify_send_fd);
    }
}

void Server::init()
{
    base = event_base_new();
    if (!base)
    {
        spdlog::error("Could not initialize libevent!");
        throw std::runtime_error("Could not initialize libevent");
    }

    if (!setupListeningSocket())
    {
        spdlog::error("Failed to set up listening socket");
        throw std::runtime_error("Failed to set up listening socket");
    }

    signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);
    sigterm_event = evsignal_new(base, SIGTERM, signal_cb, (void *)base);

    if (!signal_event || event_add(signal_event, NULL) < 0 ||
        !sigterm_event || event_add(sigterm_event, NULL) < 0)
    {
        spdlog::error("Could not create/add a signal event!");
        throw std::runtime_error("Could not create/add a signal event");
    }

    // Initialize worker threads

    worker_threads.resize(config.nthreads);
    for (int i = 0; i < config.nthreads; ++i)
    {
        int fds[2];
        if (pipe(fds))
        {
            perror("Cannot create notify pipe");
            exit(1);
        }
        worker_threads[i].notify_receive_fd = fds[0];
        worker_threads[i].notify_send_fd = fds[1];
        pthread_mutex_init(&worker_threads[i].queue_lock, nullptr);
        worker_threads[i].base = event_base_new();
        if (!worker_threads[i].base)
        {
            perror("Cannot create event base");
            exit(1);
        }

        worker_threads[i].notify_event = event_new(worker_threads[i].base, worker_threads[i].notify_receive_fd, EV_READ | EV_PERSIST, [](evutil_socket_t fd, short which, void *arg)
                                                   {
            WorkerThread *worker = static_cast<WorkerThread *>(arg);
            char buf[1];
            if (read(fd, buf, 1) != 1)
            {
                return; // spurious wakeup; the queue drain below still runs on the next notify
            }
            pthread_mutex_lock(&worker->queue_lock);
            while (!worker->conn_queue.empty())
            {
                int client_fd = worker->conn_queue.front();
                worker->conn_queue.pop();
                pthread_mutex_unlock(&worker->queue_lock);

                struct bufferevent *bev = bufferevent_socket_new(worker->base, client_fd, BEV_OPT_CLOSE_ON_FREE);
                if (!bev)
                {
                    spdlog::error("Error constructing bufferevent for client FD {}", client_fd);
                    close(client_fd);
                    continue;
                }
                bufferevent_setcb(bev, Server::conn_readcb, nullptr, Server::conn_eventcb, arg);
                bufferevent_enable(bev, EV_READ | EV_WRITE);

                pthread_mutex_lock(&worker->queue_lock);
            }
            pthread_mutex_unlock(&worker->queue_lock); }, &worker_threads[i]);
        event_add(worker_threads[i].notify_event, nullptr);

        pthread_create(&worker_threads[i].thread_id, nullptr, worker_thread_main, &worker_threads[i]);
    }

    spdlog::info("Server initialized and listening on {}:{}", config.ip, config.port);
}

bool Server::setupListeningSocket()
{
    struct sockaddr *sa;
    int sa_len;

    if (config.ip == "127.0.0.1")
    {
        // Setup for UNIX domain socket
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::string socketPath = "/tmp/server.sock";
        strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        unlink(socketPath.c_str()); // Ensure the socket path does not already exist

        sa = (struct sockaddr *)&addr;
        sa_len = sizeof(addr);
    }
    else
    {
        // Setup for TCP socket
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        if (inet_pton(AF_INET, config.ip.c_str(), &addr.sin_addr) <= 0)
        {
            spdlog::error("Invalid IP address format");
            return false;
        }

        sa = (struct sockaddr *)&addr;
        sa_len = sizeof(addr);
    }

    listener = evconnlistener_new_bind(base, listener_cb, (void *)this,
                                       LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                       sa, sa_len);

    if (!listener)
    {
        spdlog::error("Could not create a listener!");
        return false;
    }

    return true;
}

void Server::run()
{
    spdlog::info("Server running on {}:{}", config.ip, config.port);
    event_base_dispatch(base);
}

void Server::listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                         struct sockaddr *sa, int socklen, void *user_data)
{
    Server *server = static_cast<Server *>(user_data);
    spdlog::info("New connection received: FD {}", fd);
    server->dispatch_conn(fd);
}

void Server::conn_readcb(struct bufferevent *bev, void *user_data)
{
    Server *server = static_cast<Server *>(user_data);

    // Frame commands by newline: a stream read can deliver a partial command
    // or several commands at once; treating each read() as one command
    // silently corrupts pipelined or split requests.
    struct evbuffer *input = bufferevent_get_input(bev);
    char *line;
    size_t n;
    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_LF)) != nullptr)
    {
        // Strip a trailing '\r' (CRLF clients)
        if (n > 0 && line[n - 1] == '\r')
        {
            n--;
        }
        if (n > 0)
        {
            std::string command(line, n);
            server->processCommand(bev, command);
        }
        free(line);
    }

    // A client that never sends a newline must not grow the input buffer
    // without bound (largest legitimate command is a SET with a ~10KB value).
    static constexpr size_t MAX_COMMAND_BYTES = 1 << 20;
    if (evbuffer_get_length(input) > MAX_COMMAND_BYTES)
    {
        spdlog::warn("Dropping connection: unterminated command exceeds {} bytes", MAX_COMMAND_BYTES);
        bufferevent_free(bev);
    }
}

void Server::conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT))
    {
        bufferevent_free(bev);
    }
}

void Server::signal_cb(evutil_socket_t sig, short events, void *user_data)
{
    struct event_base *base = static_cast<struct event_base *>(user_data);
    struct timeval delay = {2, 0};

    spdlog::info("Exiting server cleanly in two seconds.");

    event_base_loopexit(base, &delay);
}

void Server::processCommand(struct bufferevent *bev, const std::string &command)
{
    // spdlog::debug("Processing command: {}", command);
    std::string response = processor->execute(command) + "\n";
    bufferevent_write(bev, response.c_str(), response.size());
}

void *Server::worker_thread_main(void *arg)
{
    WorkerThread *worker = static_cast<WorkerThread *>(arg);
    struct event_base *base = worker->base;

    event_base_dispatch(base);
    return nullptr;
}

void Server::dispatch_conn(int fd)
{
    // dispatch connection to a worker thread in round robin fashion
    WorkerThread &worker = worker_threads[next_worker];
    next_worker = (next_worker + 1) % worker_threads.size();

    pthread_mutex_lock(&worker.queue_lock);
    worker.conn_queue.push(fd);
    pthread_mutex_unlock(&worker.queue_lock);

    char buf[1] = {'c'};
    if (write(worker.notify_send_fd, buf, 1) != 1)
    {
        spdlog::error("Failed to write to worker notify pipe");
    }
}


static void printBanner()
{
    const char *art = R"BANNER(
   ______               __  __ ___    __
  / ____/_______  _____/ /_/ //_/ |  / /
 / /   / ___/ _ \/ ___/ __/ ,<  | | / / 
/ /___/ /  /  __(__  ) /_/ /| | | |/ /  
\____/_/   \___/____/\__/_/ |_| |___/             

)BANNER";
    fputs(art, stdout);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    printBanner();

    if (argc < 5)
    {
        std::cerr << "Usage: " << argv[0] << " <IP> <Port> <LogLevel> <Worker threads>" << std::endl;
        return 1;
    }

    std::string ip = argv[1];
    int port = std::atoi(argv[2]);
    LogLevel logLevel = static_cast<LogLevel>(std::atoi(argv[3]));
    int wthreads = std::atoi(argv[4]);

    // All translation units' globals (SODA bitmap, global_addr_start, ...)
    // are initialized by now, so guides created by the dict constructor
    // register correctly.
    processor = new ProcessCommand();

    ServerConfig config(ip, port, logLevel, wthreads);
    Server server(config);

    try
    {
        server.init();
        server.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to start server: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}