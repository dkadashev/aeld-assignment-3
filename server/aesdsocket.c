#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <stdio.h>

const char* ADDR = "127.0.0.1";
const char* PORT = "9000";
const int AF = AF_INET;
const int SOCK_TYPE = SOCK_STREAM;

const char* STORE_PATH = "/var/tmp/aesdsocketdata";

int SIGNALED = 0;

#define min(a, b) ((a) < (b) ? (a) : (b))

void
sighandler(int signum)
{
    SIGNALED = 1;
}

int
make_listener()
{
    int res;
    struct addrinfo* addr = NULL;
    struct addrinfo hints;
    int sockfd = socket(AF, SOCK_TYPE, 0);
    if (sockfd < 0) {
        syslog(LOG_ERR, "Failed to create a socket: %s", strerror(errno));
        return sockfd;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF;
    hints.ai_socktype = SOCK_TYPE;
    res = getaddrinfo(ADDR, PORT, &hints, &addr);
    if (res) {
        syslog(LOG_ERR, "getaddrinfo() failed: %s", gai_strerror(res));
        goto err;
    }
    res = bind(sockfd, addr->ai_addr, addr->ai_addrlen);
    freeaddrinfo(addr);
    if (res) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        goto err;
    }
    if (listen(sockfd, 0)) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        goto err;
    }
    return sockfd;

err:
    close(sockfd);
    return -1;
}

void
send_data_back(int clientfd, int datafd)
{
    char buf[1024];
    off_t bytes_to_go = lseek(datafd, 0, SEEK_END);
    lseek(datafd, 0, SEEK_SET);
    while (bytes_to_go > 0) {
        ssize_t res = read(datafd, buf, min(sizeof(buf), bytes_to_go));
        if (res < 0) {
            syslog(
              LOG_ERR, "Failed to read '%s': %s", STORE_PATH, strerror(errno));
            break;
        } else if (res == 0) {
            // shouldn't be here, but be cautious
            syslog(LOG_DEBUG, "EOF on the tmp file read, stopping reading");
            break;
        }
        write(clientfd, buf, res);
        bytes_to_go -= res;
    }
}

void
handle_client_conn(int clientfd, int datafd)
{
    size_t buf_size = 1024ul;
    char* buf = malloc(buf_size);
    char* data = buf;
    char* data_end = data;
    if (buf == NULL) {
        syslog(
          LOG_ERR,
          "Failed to allocate client buffer, terminating client session: %s",
          strerror(errno));
        return;
    }
    for (;;) {
        int res = recv(clientfd, data_end, buf_size - (data_end - buf), 0);
        if (res < 0) {
            syslog(LOG_ERR,
                   "Client read error, terminating client session: %s",
                   strerror(errno));
            break;
        }
        if (res == 0) {
            // XXX: should we dump the buffer into the file if it's not empty
            // here? The task description is vague here.
            syslog(LOG_DEBUG, "Client session finished");
            break;
        }
        data_end += res;
        while (data_end > data) {
            char* newline = memchr(data, '\n', data_end - data);
            if (newline == NULL) {
                break;
            }
            // We got a full packet, add it to the file and return the full file
            // contents to the client
            write(datafd, data, newline - data + 1);
            data = newline + 1;
            send_data_back(clientfd, datafd);
        }
        if (data_end < data) {
            // The whole buffer is consumed, no partial string left, reset
            data = buf;
            data_end = data;
        } else if (data != buf) {
            size_t data_size = data_end - data;
            memmove(buf, data, data_size);
            data = buf;
            data_end = data + data_size;
        } else if (data_end == buf + buf_size) {
            size_t new_buf_size = buf_size + 1024ul;
            char* new_buf = realloc(buf, new_buf_size);
            if (new_buf == NULL) {
                syslog(LOG_ERR,
                       "Failed to allocate client buffer, terminating client "
                       "session: %s",
                       strerror(errno));
                break;
            }
            buf = new_buf;
            data = buf;
            data_end = buf + buf_size;
            buf_size = new_buf_size;
        }
    }
    free(buf);
}

int
run_server_forever(int sockfd)
{
    struct sockaddr client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int datafd;
    if (unlink(STORE_PATH) && errno != ENOENT) {
        syslog(
          LOG_WARNING, "Failed to unlink %s: %s", STORE_PATH, strerror(errno));
    }
    datafd = open(STORE_PATH, O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (datafd < 0) {
        syslog(LOG_ERR, "Failed to open %s: %s", STORE_PATH, strerror(errno));
        close(sockfd);
        return 1;
    }
    syslog(LOG_DEBUG, "Listening on port %s", PORT);
    memset(&client_addr, 0, sizeof(client_addr));
    while (!SIGNALED) {
        int client_fd = accept(sockfd, &client_addr, &client_addr_len);
        if (client_fd < 0) {
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            break;
        }
        assert(client_addr.sa_family == AF_INET);
        struct sockaddr_in* addr = (struct sockaddr_in*)&client_addr;
        char buf[INET_ADDRSTRLEN];
        const char* addr_s = buf;
        if (inet_ntop(AF, &addr->sin_addr, buf, sizeof(buf)) == NULL) {
            addr_s = "<unknown>";
        }
        syslog(LOG_DEBUG, "Accepted connection from %s", addr_s);
        handle_client_conn(client_fd, datafd);
        close(client_fd);
        syslog(LOG_DEBUG, "Closed connection from %s", addr_s);
        client_addr_len = sizeof(client_addr);
    }
    syslog(LOG_DEBUG, "Caught signal, exiting");
    close(datafd);
    if (unlink(STORE_PATH) && errno != ENOENT) {
        syslog(
          LOG_WARNING, "Failed to unlink %s: %s", STORE_PATH, strerror(errno));
    }
    close(sockfd);
    return 0;
}

void
setup_sighandlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

int
do_daemonize()
{
    pid_t child_pid = fork();
    if (child_pid < 0) {
        syslog(LOG_ERR, "Failed to fork: %s", strerror(errno));
        return 1;
    } else if (child_pid == 0) {
        setsid();
        chdir("/");
        // We might want to do a second fork here to be extra careful, but for
        // this task a single fork is enough
        return 0;
    }
    return 1;
}

int
main(int argc, char* argv[])
{
    int sockfd = -1;
    int daemonize = 0;
    openlog(argv[0], LOG_PERROR | LOG_PID, LOG_USER);
    setup_sighandlers();
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "-d", 2)) {
            daemonize = 1;
        }
    }

    sockfd = make_listener();
    if (sockfd < 0) {
        return 1;
    }

    if (daemonize && do_daemonize()) {
        return 0;
    }
    return run_server_forever(sockfd);
}
