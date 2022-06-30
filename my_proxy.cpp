/*
 * my_proxy.cpp
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <vector>
#include <string>

#include "public.h"

#define PROXY_LISTEN_PORT           (22401)
#define REQUEST_BUF_SIZE            (4096)
#define BYPASS_BUF_SIZE             (4096)

#define HTTP_CONNECT_RESPONSE       "HTTP/1.1 200 Connection Established\r\n" \
                                    "Proxy-agent: my_proxy\r\n" \
                                    "\r\n"

#define DBG                         printf

typedef struct {
    st_netfd_t in;
    st_netfd_t out;
}bypass_pair;

static bool run_flag = true;

static int debug_client_count = 0;
static int debug_bypass_count = 0;

static std::string &trim_string(std::string &s)
{
    if (!s.empty()) {
        s.erase(0, s.find_first_not_of(" \r\n\t"));
        s.erase(s.find_last_not_of(" \r\n\t") + 1);
    }
    return s;
}

static void split_string(const std::string &s, std::vector<std::string> &tokens,
    const std::string &delimiters = " ")
{
    auto last_pos = s.find_first_not_of(delimiters, 0);
    auto pos = s.find_first_of(delimiters, last_pos);
    while (last_pos != std::string::npos) {
        tokens.push_back(s.substr(last_pos, pos - last_pos));
        last_pos = s.find_first_not_of(delimiters, pos);
        pos = s.find_first_of(delimiters, last_pos);
    }
}

static void parse_uri(std::string &uri, std::string &host, int &port)
{
    port = 80;
    if (uri.find("https://") != std::string::npos)
        port = 443;
    host = uri;
    if (host.find("://") != std::string::npos)
        host = host.substr(host.find("://")+3);
    if (host.find("@") != std::string::npos)
        host = host.substr(host.find("@")+1);
    if (host.find("/") != std::string::npos)
        host = host.substr(0, host.find("/"));
    if (host.find(":") != std::string::npos) {
        port = atoi(host.substr(host.find(":")+1).data());
        host = host.substr(0, host.find(":"));
    }
}

static st_netfd_t connect_to(const char *host, int port)
{
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        DBG("connect_to, socket fail\n");
        return nullptr;
    }
    st_netfd_t nfd = st_netfd_open_socket(sock);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (st_connect(nfd, (struct sockaddr *)&addr, sizeof(addr), ST_UTIME_NO_TIMEOUT) != 0) {
        DBG("connect_to, st_connect fail\n");
        st_netfd_close(nfd);
        return nullptr;
    }
    return nfd;
}

static void *proxy_bypass(void *arg)
{
    bypass_pair *pair = (bypass_pair *)arg;
    char buf[BYPASS_BUF_SIZE];
    while (run_flag) {
        int r = st_read(pair->in, buf, BYPASS_BUF_SIZE, ST_UTIME_NO_TIMEOUT);
        if (r <= 0)
            break;
        if (st_write(pair->out, buf, r, ST_UTIME_NO_TIMEOUT) != r)
            break;
    }
    st_netfd_close(pair->in);
    free(pair);
    debug_bypass_count--;
    DBG("---- debug_bypass_count: %d\n", debug_bypass_count);
    return nullptr;
}

static void *handle_client(void *arg)
{
    st_netfd_t nfd = (st_netfd_t)arg;
    int size = 0;
    char buf[REQUEST_BUF_SIZE] = {0};
    while (run_flag) {
        int r = st_read(nfd, buf+size, REQUEST_BUF_SIZE-size, ST_UTIME_NO_TIMEOUT);
        if (r <= 0) {
            DBG("handle_client, st_read fail\n");
            break;
        }
        size += r;
        const char *p = strstr(buf, "\r\n\r\n");
        if (p == nullptr)
            continue;
        std::string host;
        int port;
        std::string line = std::string(buf, strstr(buf, "\r\n")-buf);
        std::vector<std::string> req_line;
        split_string(line, req_line);
        parse_uri(req_line[1], host, port);
        DBG("handle_client, uri: %s, host: %s, port: %d\n", req_line[1].data(), host.data(), port);

        st_netfd_t proxy_nfd = connect_to(host.data(), port);
        if (proxy_nfd == nullptr) {
            DBG("handle_client, connect_to fail\n");
            break;
        }

        if (req_line[0] == "CONNECT") {
            //https
            if (st_write(nfd, HTTP_CONNECT_RESPONSE, strlen(HTTP_CONNECT_RESPONSE), ST_UTIME_NO_TIMEOUT)
                    != strlen(HTTP_CONNECT_RESPONSE)) {
                DBG("handle_client, st_write fail\n");
                st_netfd_close(proxy_nfd);
                break;
            }
        }
        else {
            //http
            if (st_write(proxy_nfd, buf, size, ST_UTIME_NO_TIMEOUT) != size) {
                DBG("handle_client, st_write fail\n");
                st_netfd_close(proxy_nfd);
                break;
            }
        }

        // client -> proxy -> server
        bypass_pair *cps = (bypass_pair *)malloc(sizeof(bypass_pair));
        cps->in = nfd;
        cps->out = proxy_nfd;
        if (st_thread_create(proxy_bypass, cps, 0, 0) == nullptr) {
            DBG("handle_client, st_thread_create fail\n");
            st_netfd_close(proxy_nfd);
            break;
        }
        debug_bypass_count++;
        // server -> proxy -> client
        bypass_pair *spc = (bypass_pair *)malloc(sizeof(bypass_pair));
        spc->in = proxy_nfd;
        spc->out = nfd;
        if (st_thread_create(proxy_bypass, spc, 0, 0) == nullptr) {
            DBG("handle_client, st_thread_create fail\n");
            st_netfd_close(proxy_nfd);
            break;
        }
        debug_bypass_count++;
        debug_client_count--;
        DBG("---- debug_client_count: %d\n", debug_client_count);
        return nullptr;
    }
    st_netfd_close(nfd);
    debug_client_count--;
    DBG("---- debug_client_count: %d\n", debug_client_count);
    return nullptr;
}

int main(int argc, char **argv)
{
    if (st_set_eventsys(ST_EVENTSYS_ALT) != 0) {
        DBG("st_set_eventsys fail\n");
        return 0;
    }
    if (st_init() != 0) {
        DBG("st_init fail\n");
        return 0;
    }
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        DBG("socket fail\n");
        return 0;
    }
    int n = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(int)) != 0) {
        DBG("setsockopt fail\n");
        close(sock);
        return 0;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROXY_LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) != 0) {
        DBG("bind fail\n");
        close(sock);
        return 0;
    }
    if (listen(sock, 128) != 0) {
        DBG("listen fail\n");
        close(sock);
        return 0;
    }
    DBG("proxy listen %d ...\n", PROXY_LISTEN_PORT);
    st_netfd_t srv_nfd = st_netfd_open_socket(sock);
    while (run_flag) {
        struct sockaddr_in cli_addr;
        int n = sizeof(struct sockaddr_in);
        st_netfd_t cli_nfd = st_accept(srv_nfd, (struct sockaddr *)&cli_addr, &n, ST_UTIME_NO_TIMEOUT);
        if (cli_nfd != nullptr) {
            if (st_thread_create(handle_client, cli_nfd, 0, 0) == nullptr) {
                DBG("st_thread_create fail\n");
                break;
            }
            debug_client_count++;
        }
        else {
            DBG("st_accept fail\n");
            break;
        }
    }
    st_netfd_close(srv_nfd);
    DBG("proxy stop\n");
    return 0;
}

