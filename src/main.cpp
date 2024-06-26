#include <stdio.h> 
#include <netdb.h> 
#include <netinet/in.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <unistd.h> 
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#define PORT 8100

#include <iostream>
#include <vector>
#include <algorithm>
#include <cassert>

#include "microtunnel/common.h"
#include "kc1fsz-tools/Common.h"
#include "kc1fsz-tools/IPAddress.h"
#include "kc1fsz-tools/Log.h"

using namespace std;
using namespace kc1fsz;

namespace kc1fsz {
void panic(const char* msg) {
    cerr << msg << endl;
    assert(false);
}
}

struct Proxy {

    void close() {
        if (fd != 0) {
            cout << "Closing proxy " << clientId << endl;
            ::close(fd);
        }
    }

    // The id assigned by the client for this proxy
    uint16_t clientId = 0;
    // The actual socket used
    int fd = 0;
    bool isDead = false;
    enum Type { UNKNOWN, TCP, UDP, } type = Type::UNKNOWN;
};

struct Client {

    void close() {
        // Close all proxies
        for (Proxy p : proxies) p.close();
        if (fd != 0) {
            cout << "Closing client" << endl;
            ::close(fd);
        }
    }

    void cleanup() {
        if (!proxies.empty()) {
            for (Proxy p : proxies) {
                if (p.isDead) p.close();
            }
            proxies.erase(
                std::remove_if(
                    proxies.begin(), 
                    proxies.end(),
                    [](const Proxy& x) { return x.isDead; }
                ),
                proxies.end()
            );
        }

    }

    // The socket back to the client
    int fd = 0;
    // The address of the client
    sockaddr_in addr;
    bool isDead = false;

    // The recBuf is used to accumulate data from the client
    const static uint16_t recBufSize = 2048;
    uint8_t recBuf[recBufSize];
    uint16_t recBufLen = 0;

    uint16_t getFrameLen() const { return recBuf[0] << 8 | recBuf[1]; }

    // All of the active proxies for this client
    std::vector<Proxy> proxies;
};

static void sendTCPOpenRespToClient(Client& client, uint16_t clientId, uint8_t c) {
    uint8_t header[8];
    uint16_t totalLen = 6;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_OPEN_TCP;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    header[5] = c;
    // TODO: ERROR
    write(client.fd, header, totalLen);
}

static void sendCloseRespToClient(Client& client, uint16_t clientId, uint8_t c) {
    uint8_t header[8];
    uint16_t totalLen = 6;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_CLOSE;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    header[5] = c;
    // TODO: ERROR
    write(client.fd, header, totalLen);
}

static void sendTCPSendRespToClient(Client& client, uint16_t clientId) {
    uint8_t header[8];
    uint16_t totalLen = 5;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_SEND_TCP;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    // TODO: ERROR
    write(client.fd, header, totalLen);
}

static void sendTCPRecvRespToClient(Client& client, uint16_t clientId, const uint8_t* data, uint16_t dataLen) {
    uint8_t header[8];
    uint16_t totalLen = 5 + dataLen;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_RECV_TCP;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    // TODO: ERROR
    write(client.fd, header, 5);
    write(client.fd, data, dataLen);
}

static void sendRecvDataToClient(Client& client, uint16_t id, const uint8_t* data, uint16_t dataLen,
    const IPAddress& addr, uint16_t port) {

    if (dataLen > 2048) {
        panic("Data too long");
        return;
    }

    RecvData frame;
    frame.len = 12 + dataLen;
    frame.type = ClientFrameType::RECV_DATA;
    frame.id = id;
    frame.addr = addr.getAddr();
    frame.port = port;
    memcpyLimited(frame.data, data, dataLen, sizeof(frame.data));
    write(client.fd, &frame, frame.len);
}

static void sendDNSQueryRespToClient(Client& client, const char* hostName, uint32_t addr, uint8_t c) {
    uint8_t header[16];
    uint16_t totalLen = 8;
    if (hostName != 0) {
        totalLen += strlen(hostName);
    }
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_QUERY_DNS;
    header[3] = c;
    header[4] = (addr & 0xff000000) >> 24;
    header[5] = (addr & 0x00ff0000) >> 16;
    header[6] = (addr & 0x0000ff00) >> 8;
    header[7] = (addr & 0x000000ff);
    // TODO: ERROR
    write(client.fd, header, 8);
    if (hostName != 0)
        write(client.fd, hostName, strlen(hostName));
}

static void sendUDPBindRespToClient(Client& client, uint16_t id, uint16_t rc) {
    ResponseBindUDP resp;
    resp.len = sizeof(ResponseBindUDP);
    resp.type = ClientFrameType::RESP_BIND_UDP;
    resp.id = id;
    resp.rc = rc;
    write(client.fd, &resp, resp.len);
}

static void processClientFrame(Client& client, const uint8_t* frame, uint16_t frameLen,
    Log* log) {

    const uint16_t reqLen = a_ntohs(*((uint16_t*)frame));
    const uint16_t reqType = a_ntohs(*((uint16_t*)(frame + 2)));

    // Sanity check
    if (reqLen != frameLen) {
        log->error("Length error");
        return;
    }

    if (reqType == ClientFrameType::REQ_PING) {
    }
    else if (reqType == ClientFrameType::REQ_RESET) {
    }
    else if (reqType == ClientFrameType::REQ_OPEN_TCP) {

        RequestOpenTCP req;
        memcpy(&req, frame, std::min((unsigned int)frameLen, (unsigned int)sizeof(req)));

        Proxy proxy;
        proxy.type = Proxy::Type::TCP;
        proxy.clientId = req.clientId;
        uint32_t targetAddr = req.addr;
        uint16_t targetPort = req.port;

        proxy.fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in target; 
        target.sin_family = AF_INET; 
        target.sin_addr.s_addr = htonl(targetAddr); 
        target.sin_port = htons(targetPort); 

        char buf[32];
        inet_ntop(AF_INET, &(target.sin_addr.s_addr), buf, 32);
        log->info("TCP connecting to %s:%d (%u)", buf, targetPort, proxy.clientId);

        int rc = connect(proxy.fd, (sockaddr*)&target, sizeof(target));
        if (rc != 0) {
            close(proxy.fd);
            // Send back an error
            sendTCPOpenRespToClient(client, proxy.clientId, 1);
        } 
        else {
            client.proxies.push_back(proxy);
            // Send a success message
            sendTCPOpenRespToClient(client, proxy.clientId, 0);
        }
    }
    else if (reqType == ClientFrameType::REQ_SEND_TCP) {

        // TODO: SIZE CHECK
        RequestSendTCP req;
        memcpy(&req, frame, std::min((unsigned int)frameLen, (unsigned int)sizeof(req)));
        //log->debugDump("Sending TCP data", req.contentPlaceholder, frameLen - 5);

        for (Proxy& proxy : client.proxies) {
            if (proxy.clientId == req.clientId) {
                // TODO: ERROR?                
                write(proxy.fd, req.contentPlaceholder, frameLen - 5);
                // Send a success message
                sendTCPSendRespToClient(client, proxy.clientId);
                return;
            }
        }
        // TODO: ERROR
    }
    else if (reqType == ClientFrameType::REQ_SEND_UDP) {

        if (frameLen < 12) {
            log->error("Invalid request ignored");
            return;
        }

        RequestSendUDP req;
        const uint32_t dataLen = frameLen - 12;
        memcpyLimited((uint8_t*)&req, frame, frameLen, (unsigned int)sizeof(req));

        //log->debugDump("Sending UDP data", req.data, dataLen);
        log->info("Sending UDP data");

        struct sockaddr_in peerAddr; 
        memset(&peerAddr, 0, sizeof(peerAddr)); 
        peerAddr.sin_family = AF_INET; 
        peerAddr.sin_addr.s_addr = htonl(req.addr);
        peerAddr.sin_port = htons(req.port); 
    
        auto it = std::find_if(client.proxies.begin(), client.proxies.end(), 
            [&req](const Proxy& x) { return x.clientId == req.id; });
        if (it != client.proxies.end()) {
            sendto(it->fd, req.data, dataLen, 0, 
                (const struct sockaddr *)&peerAddr, sizeof(peerAddr));
            // TODO: ERRORS?
        }
    }
    else if (reqType == ClientFrameType::REQ_QUERY_DNS) {

        RequestQueryDNS req;
        memcpy(&req, frame, std::min((unsigned int)frameLen, (unsigned int)sizeof(req)));

        struct hostent* he = gethostbyname(req.name);
        if (he == 0) {
            // Send a failure message
            sendDNSQueryRespToClient(client, 0, 0, 1);
        } else {
            const in_addr** addr_list = (const in_addr **)he->h_addr_list;
            if (addr_list[0] == 0) {
                // Send a failure message
                sendDNSQueryRespToClient(client, 0, 0, 1);
            } else {
                sendDNSQueryRespToClient(client, req.name, ntohl(addr_list[0]->s_addr), 0);                
            }
        }
    }
    else if (reqType == ClientFrameType::REQ_BIND_UDP) {

        RequestBindUDP req;
        memcpy(&req, frame, std::min((unsigned int)frameLen, (unsigned int)sizeof(req)));

        Proxy proxy;
        proxy.type = Proxy::Type::UDP;
        proxy.clientId = req.id;

        proxy.fd = socket(AF_INET, SOCK_DGRAM, 0);
        
        struct sockaddr_in serverAddr; 
        memset(&serverAddr, 0, sizeof(serverAddr)); 
        serverAddr.sin_family = AF_INET; 
        serverAddr.sin_addr.s_addr = INADDR_ANY; 
        serverAddr.sin_port = htons(req.bindPort); 

        if (bind(proxy.fd, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
            log->error("Failed to bind id %d", req.id);
            close(proxy.fd);
            // Send back an error
            sendUDPBindRespToClient(client, req.id, 1);
        }
        else {
            log->info("Bound id %d on port", req.id, req.bindPort);
            client.proxies.push_back(proxy);
            sendUDPBindRespToClient(client, req.id, 0);
        }        
    }
    else {
        log->error("Unrecognized request %d", reqType);
    }
}

int main(int argc, const char** argv) {

    Log log;
    log.info("MicroTunnel 2024-04-01");

    std::vector<Client> clients;

    // Open a socket to receive client connections
    struct sockaddr_in serverAddr; 
   
    // socket create and verification 
    int serverFd = socket(AF_INET, SOCK_STREAM, 0); 
    if (serverFd == -1) { 
        log.error("Server socket creation failed");
        return -1;
    } 

    int v = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(int)) < 0)
        log.error("setsockopt(SO_REUSEADDR) failed");

    bzero(&serverAddr, sizeof(serverAddr));    
    // assign IP, PORT 
    serverAddr.sin_family = AF_INET; 
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serverAddr.sin_port = htons(PORT); 
   
    // Binding newly created socket to given IP and verification 
    if ((bind(serverFd, (sockaddr*)&serverAddr, sizeof(serverAddr))) != 0) { 
        cerr << "Server socket bind failed" << endl;
        return -2;
    } 
   
    // Now server is ready to listen and verification 
    if ((listen(serverFd, 5)) != 0) { 
        cerr << "Server socket listen failed" << endl;
        return -2;
    } 
   
    // Make the listening socket non-blocking
    int flags = fcntl(serverFd, F_GETFL, 0);
    if (flags == -1) return -1;
    flags = (flags | O_NONBLOCK);
    fcntl(serverFd, F_SETFL, flags);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500;
    fd_set rfds, wfds;

    while (true) {

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(serverFd, &rfds);
        int maxFd = serverFd;

        // Add clients
        for (const Client& client : clients) {
            maxFd = std::max(maxFd, client.fd);
            FD_SET(client.fd, &rfds);
            // Add proxies
            for (const Proxy& proxy : client.proxies) {
                maxFd = std::max(maxFd, proxy.fd);
                FD_SET(proxy.fd, &rfds);
            }
        }

        int active = select(maxFd + 1, &rfds, &wfds, NULL, &tv);
        if (active > 0) {

            // Check for inbound connection requests
            if (FD_ISSET(serverFd, &rfds)) {

                struct sockaddr_in clientAddr; 
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &addrLen); 
                if (clientFd < 0) { 
                    log.error("Client accept failed");
                } 
                else {
                    // Make client non-blocking
                    int flags = fcntl(clientFd, F_GETFL, 0);
                    flags = (flags | O_NONBLOCK);
                    fcntl(serverFd, F_SETFL, flags);

                    /*
                    // Turn on keep-alive
                    int yes = 1;
                    setsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(int));
                    // Idle interval before probes start
                    int idle = 10;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(int));
                    // Time between probes
                    int interval = 5;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(int));
                    // Number of bad probes before we give up
                    int maxpkt = 5;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(int));
                    */

                    // Mark all other clients as dead - we should only have one
                    for (Client& client : clients)
                        client.isDead = true;

                    clients.push_back({ clientFd, clientAddr });
                    char buf[32];
                    inet_ntop(AF_INET, &(clients.at(0).addr.sin_addr), buf, 32);
                    log.info("New client from %s", buf);
                }
            }

            // Check all clients for activity
            for (Client& client : clients) {
                
                if (FD_ISSET(client.fd, &rfds)) {
                    // Decide how much to read
                    uint16_t maxReadSize;
                    if (client.recBufLen == 0) {
                        maxReadSize = 2;
                    } else if (client.recBufLen == 1) {
                        maxReadSize = 1;
                    } else {
                        maxReadSize = client.getFrameLen() - client.recBufLen;
                    }
                    int rc = read(client.fd, client.recBuf + client.recBufLen, maxReadSize);
                    if (rc <= 0) {
                        client.isDead = true;
                        log.info("Client disconnected");
                    } else {
                        client.recBufLen += rc;
                        // Do we have a complete frame?
                        if (client.recBufLen >= 2 && client.recBufLen == client.getFrameLen()) {
                            processClientFrame(client, client.recBuf, client.recBufLen, &log);
                            // Reset for new frame
                            client.recBufLen = 0;
                        }
                    }
                }

                // Check all proxies for activity
                for (Proxy& proxy : client.proxies) {
                    if (FD_ISSET(proxy.fd, &rfds)) {

                        if (proxy.type == Proxy::Type::TCP) {
                            uint8_t buf[256];
                            int rc = read(proxy.fd, (char*)buf, 256);
                            // Proxy disconnect case
                            if (rc <= 0) {
                                proxy.isDead = true;
                                log.info("Proxy %u disconnected", proxy.clientId);
                                sendCloseRespToClient(client, proxy.clientId, 0);
                            } 
                            // Proxy received data, send it back to the client
                            else {
                                log.info("Received TCP data from proxy %u", proxy.clientId);
                                //log.debugDump("Data", buf, rc);
                                sendTCPRecvRespToClient(client, proxy.clientId, buf, rc);
                            }
                        } 
                        else if (proxy.type == Proxy::Type::UDP) {
                            uint8_t buf[1024];
                            struct sockaddr_in peerAddr;         
                            socklen_t peerAddrLen = sizeof(peerAddr);            
                            int rc = recvfrom(proxy.fd, (char*)buf, sizeof(buf), MSG_WAITALL,
                                (sockaddr*)&peerAddr, &peerAddrLen);
                            // Proxy disconnect/error case
                            if (rc <= 0) {
                                proxy.isDead = true;
                            } 
                            // Proxy received data, send it back to the client
                            else {
                                IPAddress addr(ntohl(peerAddr.sin_addr.s_addr));
                                uint16_t port = ntohs(peerAddr.sin_port);
                                log.info("Received UDP data from proxy for id %u len %d", 
                                    proxy.clientId, rc);
                                //log.debugDump("Data", buf, rc);
                                sendRecvDataToClient(client, proxy.clientId, buf, rc, addr, port);
                            }
                        }
                    }
                }
            }
        }

        // Cleanup dead proxies
        for (Client& client : clients) client.cleanup();

        // Cleanup dead clients
        if (!clients.empty()) {
            for (Client& client : clients) {
                if (client.isDead) client.close();
            }
            clients.erase(
                std::remove_if(
                    clients.begin(), 
                    clients.end(),
                    [](const Client& x) { return x.isDead; }
                ),
                clients.end()
            );
        }
    }
}
