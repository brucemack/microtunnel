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

#define PORT 8100

#include <iostream>
#include <vector>
#include <algorithm>
#include <cassert>

#include "microtunnel/common.h"
#include "kc1fsz-tools/Common.h"

using namespace std;
using namespace kc1fsz;

namespace kc1fsz {
void panic(const char* msg) {
    cerr << msg << endl;
    assert(false);
}
}

struct Proxy {
    // The id assigned by the client for this proxy
    uint16_t clientId;
    // The actual socket used
    int fd;
    bool isDead = false;
    enum Type { UNKNOWN, TCP, UDP, } type;
};

struct Client {
    // The socket back to the client
    int fd;
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
    int rc = write(client.fd, header, totalLen);
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
    int rc = write(client.fd, header, totalLen);
}

static void sendTCPSendRespToClient(Client& client, uint16_t clientId) {
    uint8_t header[8];
    uint16_t totalLen = 5;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_SEND_TCP;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    int rc = write(client.fd, header, totalLen);
}

static void sendTCPRecvRespToClient(Client& client, uint16_t clientId, const uint8_t* data, uint16_t dataLen) {
    uint8_t header[8];
    uint16_t totalLen = 5 + dataLen;
    header[0] = (totalLen & 0xff00) >> 8;
    header[1] = (totalLen & 0x00ff);
    header[2] = ClientFrameType::RESP_RECV_TCP;
    header[3] = (clientId & 0xff00) >> 8;
    header[4] = clientId & 0x00ff;
    int rc = write(client.fd, header, 5);
    rc = write(client.fd, data, dataLen);
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
    int rc = write(client.fd, header, 8);
    if (hostName != 0)
        rc = write(client.fd, hostName, strlen(hostName));
}

static void processClientFrame(Client& client, const uint8_t* frame, uint16_t frameLen) {

    const uint16_t reqLen = a_ntohs(*((uint16_t*)frame));
    const uint16_t reqType = a_ntohs(*((uint16_t*)(frame + 2)));

    // Sanity check
    if (reqLen != frameLen) {
        cout << "Length error" << endl;
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
        cout << "TCP connecting to " << buf << ":" << targetPort 
            << " (" << proxy.clientId << ")" << endl;

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
        prettyHexDump(req.contentPlaceholder, frameLen - 5, cout);

        for (Proxy& proxy : client.proxies) {
            if (proxy.clientId == req.clientId) {
                int rc = write(proxy.fd, req.contentPlaceholder, frameLen - 5);
                // Send a success message
                sendTCPSendRespToClient(client, proxy.clientId);
                return;
            }
        }
        // TODO: ERROR
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
    else {
        cout << "Unrecognized request: " << reqType << endl;
    }
}

int main(int argc, const char** argv) {

    std::vector<Client> clients;

    // Open a socket to receive client connections
    struct sockaddr_in serverAddr; 
   
    // socket create and verification 
    int serverFd = socket(AF_INET, SOCK_STREAM, 0); 
    if (serverFd == -1) { 
        cerr << "Server socket creation failed" << endl;
        return -1;
    } 

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

    int waitms = 2;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = waitms * 1000;
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
                    cout << "Client accept failed" << endl;
                } 
                else {
                    // Make client non-blocking
                    int flags = fcntl(clientFd, F_GETFL, 0);
                    flags = (flags | O_NONBLOCK);
                    fcntl(serverFd, F_SETFL, flags);

                    clients.push_back({ clientFd, clientAddr });
                    char buf[32];
                    inet_ntop(AF_INET, &(clients.at(0).addr.sin_addr), buf, 32);
                    cout << "Client from " << buf << endl;
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
                    if (rc <= 0)                     {
                        cout << "Client disconnected" << endl;
                        client.isDead = true;
                        close(client.fd);
                    } else {
                        client.recBufLen += rc;
                        // Do we have a complete frame?
                        if (client.recBufLen >= 2 && client.recBufLen == client.getFrameLen()) {
                            processClientFrame(client, client.recBuf, client.recBufLen);
                            // Reset for new frame
                            client.recBufLen = 0;
                        }
                    }
                }

                // Check all proxies for activity
                for (Proxy& proxy : client.proxies) {
                    if (FD_ISSET(proxy.fd, &rfds)) {
                        uint8_t buf[256];
                        int rc = read(proxy.fd, (char*)buf, 256);
                        // Proxy disconnect case
                        if (rc <= 0) {
                            close(proxy.fd);
                            proxy.fd = 0;
                            proxy.isDead = true;
                            cout << "Sending disconnect to client" << endl;
                            sendCloseRespToClient(client, proxy.clientId, 0);
                        } 
                        // Proxy received data, send it back to the client
                        else {
                            sendTCPRecvRespToClient(client, proxy.clientId, buf, rc);
                        }
                    }
                }
            }
        }

        // Cleanup dead proxies
        for (Client& client : clients) {
            if (!client.proxies.empty())
                client.proxies.erase(
                    std::remove_if(
                        client.proxies.begin(), 
                        client.proxies.end(),
                        [](const Proxy& x) { return x.isDead; }
                    ),
                    client.proxies.end()
                );
        }

        // Cleanup dead clients
        if (!clients.empty())
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
