#include<iostream>
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <sstream>
#include <chrono>

using namespace std;

class ProxyServer {
    private:
        int port;
        SOCKET serverSocket;

        struct cacheContainer {
            string resp;
            chrono::steady_clock::time_point req_timestamp;
            int maxAge; // in seconds
            bool checkValidity() const {
                auto now = chrono::steady_clock::now();
                // calculate the time that has passed since the saving of the request in the cache
                auto elapsed = chrono::duration_cast<chrono::seconds>(now - req_timestamp);
                return elapsed.count() < maxAge;
            }
        };

        int cacheMaxSize = 4; 
        list<string> lruList; // stores keys in recency order
        unordered_map<string, pair<cacheContainer, list<string>::iterator>> cacheMapping;
        // Use mutex for locking the cache operations in case multiple threads are used
        mutex cacheMutex;
        int defCacheTime = 300; // 5 minutes

        struct HttpReq {
            string method, url, host, version, body;
            vector<string> headers;
            bool isValid = false;
            bool isCacheable = false; // GET  & HEAD methods are cacheable
        };

        struct HttpResp {
            string statusLine, body, fullResp;
            vector<string> headers;
            int maxAge = 0;
            bool isCacheable = false;
        };

        HttpReq parseHttpReq(const string & request) {
            HttpReq req;
            // Converting string into an input stream
            istringstream stream(request);
            string line;

            // Parse the request line
            if(getline(stream, line)){
                istringstream requestLine(line);
                // Getting request information from the stream
                requestLine >> req.method >> req.url >> req.version;

                if(!req.version.empty() && req.version.back() == '\r'){
                    req.version.pop_back();
                }

                
                // Check for request validity
                req.isValid = !req.method.empty() && !req.url.empty();

                req.isCacheable = (req.method == "GET");
            }

            // Parse headers
            while(getline(stream, line) && line != "\r" && !line.empty()){
                req.headers.push_back(line);

                // extract host header
                if(line.find("Host:") == 0) {
                    req.host = line.substr(5);
                    if(!req.host.empty() && req.host[0] == ' '){
                        req.host = req.host.substr(1);
                    }
                    if(!req.host.empty() && req.host.back() == '\r'){
                        req.host.pop_back();
                    }
                }
                // check for cacheability
                if(line.find("Cache-Control") == 0 && line.find("no-cache") != string::npos){
                    req.isCacheable = false;
                }
            }

            string bodyLine;

            while(getline(stream, bodyLine)) {
                req.body += bodyLine + "\n";
            }

            return req;
        }

        HttpResp parseHttpResponse(const string& response){
            HttpResp resp;

            istringstream stream(response);
            string line;

            if(getline(stream, line)) {
                resp.statusLine = line;

                if(line.back() == '\r'){
                    line.pop_back();
                }

                resp.isCacheable = (line.find("200 OK") != string::npos);
            }

            while(getline(stream, line) && line != "\r" && !line.empty()){
                resp.headers.push_back(line);

                if(line.find("Cache-control:") == 0) {
                    if(line.find("no-cache") != string::npos || line.find("no-store") != string::npos){
                        resp.isCacheable =  false;
                    } 
                }

                // set maxAge of the response
                size_t maxAgeIdx = line.find("max-age=");

                if(maxAgeIdx != string::npos){
                    string age = line.substr(maxAgeIdx + 8);
                    size_t commaIdx = age.find(',');
                    if(commaIdx != string::npos){
                        age = age.substr(0, commaIdx);
                    }
                    resp.maxAge = stoi(age);
                }

                if(line.find("Expires:") == 0){
                    resp.isCacheable = true;
                }
            }

            string bodyLine;

            while(getline(stream, bodyLine)){
                resp.body += bodyLine + "\n";
            }
            resp.fullResp = response;
            return resp;
        }
        
        // generate unique identifier for a request in the cacheMap
        string genCacheKey(const HttpReq& req){
            return req.method + ":" + req.host + req.url;
        }

        // search for a cache response in the cacheMap
        bool getCacheResponse(const string& cacheKey, string& response){
            lock_guard<mutex> lock(cacheMutex);

            auto it = cacheMapping.find(cacheKey);

            if(it != cacheMapping.end() && it->second.first.checkValidity()){
                lruList.erase(it->second.second);
                lruList.push_front(cacheKey);

                it->second.second = lruList.begin();

                response = it->second.first.resp;
                cout <<"Cache HIT for: "<<cacheKey<<endl;
                return true;
            }

            if(it != cacheMapping.end() && !it->second.first.checkValidity()){
                lruList.erase(it->second.second);
                cacheMapping.erase(it);
                cout<<"Cache EXPIRED for: "<<cacheKey<<endl;
            }

            cout<<"Cache MISS for: "<<cacheKey<<endl;
            return false;
        }

        // Fill the cached response for the request in the cacheMap
        void cacheResponse(const string& cacheKey, const string& response, int maxAge){
            lock_guard<mutex> lock(cacheMutex);

            cacheContainer entry;
            entry.resp = response;
            entry.req_timestamp = chrono::steady_clock::now();
            entry.maxAge = (maxAge > 0) ? maxAge: defCacheTime;

            lruList.push_front(cacheKey);
            cacheMapping[cacheKey] = {entry, lruList.begin()};


            if(cacheMapping.size() > cacheMaxSize){
                string lruKey = lruList.back();
                lruList.pop_back();
                cacheMapping.erase(lruKey);
                cout<<"Cleared LRU cache entry"<<endl;
            }
            cout<<"Cached response for: "<<cacheKey<<" (TTL: "<<entry.maxAge<<"s)"<<endl;
        }

        // Print current stat of the cacheMap
        void printCacheStats() {
            lock_guard<mutex> lock(cacheMutex);

            cout<<"Cache contains "<<cacheMapping.size()<<" entries"<<endl;

            int validEntries = 0;

            for(const auto& pair: cacheMapping){
                if(pair.second.first.checkValidity()){
                    validEntries++;
                }
            }
            cout<<"Valid entries: "<<validEntries<<endl;
        }

        // Connect to target server
        SOCKET connectToTargetServer(const string& host, int port = 80) {
            SOCKET targetSocket = socket(AF_INET, SOCK_STREAM, 0);

            if(targetSocket == INVALID_SOCKET){
                cout<<"Failed to create target Socket"<<endl;
                return INVALID_SOCKET;
            }

            sockaddr_in target;

            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            // convert from string (www.google.com) to IP-address (8.8.8.8) and check validity
            if(inet_addr(host.c_str()) == INADDR_NONE){
                struct addrinfo hints, *result;
                ZeroMemory(&hints, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;

                // if conversion fails, show error and close the socket
                if(getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &result) != 0){
                    cout<<"Failed to resolve hostname: "<<host<<" (Error: "<<WSAGetLastError()<<")"<<endl;
                    closesocket(targetSocket);
                    return INVALID_SOCKET;
                }

                target.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
                freeaddrinfo(result);
            }
            else{
                target.sin_addr.s_addr = inet_addr(host.c_str());
            }

            // set timeout for send and receive operations
            int timeout = 5000;
            setsockopt(targetSocket, SOL_SOCKET, SO_RCVTIMEO, (char*) &timeout, sizeof(timeout));
            setsockopt(targetSocket, SOL_SOCKET, SO_SNDTIMEO, (char*) &timeout, sizeof(timeout));

            if(connect(targetSocket, (sockaddr*)&target, sizeof(target)) == SOCKET_ERROR){
                cout<<"Failed to connect to target: "<<host<<":"<<port<<endl;
                closesocket(targetSocket);
                return INVALID_SOCKET;
            }

            return targetSocket;
        }

        // Function to handle each new client connection
        void handleClient(SOCKET clientSocket){
            char buffer[8192];
            string reqData;

            cout<<"Handling new client connection."<<endl;

            // Loop to receive full HTTP request from the client
            while(true){
                // receive data from the client
                int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                if(bytesReceived == SOCKET_ERROR){
                    int error = WSAGetLastError();
                    if(error == WSAETIMEDOUT){
                        cout<<"Client timeout"<<endl;
                    }
                    else{
                        cout<<"Error receiving from the client"<<endl;
                    }
                    break;
                } else if (bytesReceived == 0){
                    cout<<"Client disconnected"<<endl;
                    break;
                }

                buffer[bytesReceived] = '\0'; // Null-terminate the received data
                reqData += string(buffer, bytesReceived);

                // Check if the end of the HTTP request headers is found ("\r\n\r\n")
                if(reqData.find("\r\n\r\n") != string::npos){
                    HttpReq req = parseHttpReq(reqData);

                    // Reject CONNECT method as HTTPS is not supported
                    if (req.method == "CONNECT") {
                        cout << "CONNECT not supported, rejecting request" << endl;
                        string errorResp = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                        send(clientSocket, errorResp.c_str(), errorResp.length(), 0);
                        break;
                    }

                    
                    if(!req.isValid){
                        cout<<"Invalid HTTP request"<<endl;
                        string errorResp = "HTTP/1.1 404 Bad Request\r\n\r\n";
                        send(clientSocket, errorResp.c_str(), errorResp.length(), 0);
                        break;
                    }

                    // Log details of the incoming request.
                    cout<<"Request: "<<req.method<<" "<<req.url<<" "<<req.version<<endl;
                    cout<<"Host:"<<req.host<<endl;

                    string cacheKey = genCacheKey(req); // Generate a unique key for caching
                    string cachedResp;

                    
                    if(req.isCacheable && getCacheResponse(cacheKey, cachedResp)){
                        // Serve the response directly from the cache to the client
                        send(clientSocket, cachedResp.c_str(), cachedResp.length(), 0);
                        cout<<"served from cache"<<endl;
                    }
                    else{
                        // Forward the request to the Target Server
                        SOCKET targetSocket = connectToTargetServer(req.host);

                        if(targetSocket == INVALID_SOCKET){
                            string errorResp = "HTTP/1.1 502 Bad gateway \r\n\r\n";
                            send(clientSocket, errorResp.c_str(), errorResp.length(), 0);
                            break;
                        }

                        // Send the client's request to the target server
                        if(send(targetSocket, reqData.c_str(), reqData.length(), 0) == SOCKET_ERROR){
                            cout<<"Failed to send request to the target server"<<endl;
                            closesocket(targetSocket);
                            break;
                        }

                        cout<<"Forwarded request to the target server"<<endl;

                        string fullResp;
                        bool connectionClosed = false;

                        while(true){
                            // Receive the response from target server
                            int serverBytes = recv(targetSocket, buffer, sizeof(buffer) - 1, 0);

                            if(serverBytes == SOCKET_ERROR){
                                int error = WSAGetLastError();
                                if(error == WSAETIMEDOUT){
                                    cout<<"Target server timeout"<<endl;
                                }
                                else{
                                    cout<<"Error receiving from target server: "<<error<<endl;
                                }
                                break;
                            }
                            else if (serverBytes == 0){
                                cout<<"Target server closed connection"<<endl;
                                connectionClosed = true;
                                break;
                            }

                            buffer[serverBytes] = '\0';
                            fullResp += string(buffer, serverBytes);

                            int totalSent = 0;
                            
                            // Forward the received response data to the client
                            while(totalSent < serverBytes){
                                int sent = send(clientSocket, buffer + totalSent, serverBytes - totalSent, 0);
                                if(sent == SOCKET_ERROR){
                                    cout<<"Failed to send response to client";
                                    connectionClosed = true;
                                    break;
                                }
                                totalSent += sent;
                            }

                            if(connectionClosed) break;
                        }

                        closesocket(targetSocket);

                        // If request was cacheable and a full response was received, attempt to cache it
                        if(req.isCacheable && !fullResp.empty()){
                            HttpResp resp = parseHttpResponse(fullResp);
                            if(resp.isCacheable){
                                int maxAge = (resp.maxAge > 0)? maxAge: defCacheTime;
                                cacheResponse(cacheKey, fullResp, maxAge);
                            }
                        }
                    }

                    bool keepAlive = false;

                    for(const string& header: req.headers){
                        if(header.find("Connection:") == 0 && header.find("keep-alive") != string::npos){
                            keepAlive = true;
                            break;
                        }
                    }

                    if(!keepAlive || req.version == "HTTP/1.0"){
                        break;
                    }

                    reqData.clear();
                }
            }

            closesocket(clientSocket);
            cout<<"Client connection closed"<<endl;
        }

public:
    ProxyServer(int port, int cacheTime = 300) : port(port), serverSocket(INVALID_SOCKET), defCacheTime(cacheTime){}

    bool start(){
        WSADATA wsa;
        // Initialize Winsock
        if(WSAStartup(MAKEWORD(2,2), &wsa) != 0){
            cout<<"Failed to initialize Winsock"<<endl;
        }

        // Create the server listening socket
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);

        if(serverSocket == INVALID_SOCKET){
            cout<<"Failed to creater server soccket";
            WSACleanup();
            return false;
        }

        // Set socket option SO_REUSEADDR to allow the socket to be bound to an address
        // that is already in use by a previous connection in TIME_WAIT state
        // This helps in restarting the server quickly after a crash or restart

        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        // Fill in the details of the server's address structure (sockaddr_in)
        sockaddr_in server;

        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons(port);
        server.sin_family = AF_INET;

        // Bind the server socket to the specified IP address and port
        if(bind(serverSocket, (sockaddr*) &server, sizeof(server)) == SOCKET_ERROR){
            cout <<"Failed to bind socket"<<endl;
            closesocket(serverSocket);
            WSACleanup();
            return false;
        }
        // Put the server socket into a listening state
        if(listen(serverSocket, 5) == SOCKET_ERROR){
            cout<<"Failed to listen on the server socket"<<endl;
            closesocket(serverSocket);
            WSACleanup();
            return false;
        }

        cout << "Proxy server listening on port " << port << endl;
        cout << "Default cache time: " << defCacheTime << " seconds" << endl;

        // Create a detached thread for periodic cache statistics logging
        thread statsThread([this](){
            while(true){
                this_thread::sleep_for(chrono::seconds(30));
                this->printCacheStats();
            }
        });
        statsThread.detach();

        while(true){
            sockaddr_in client; // Structure to hold client address details
            int clientSize = sizeof(client);
            // Accept a new incoming client connection.
            SOCKET clientSocket = accept(serverSocket, (sockaddr*)&client, &clientSize);
                
            if (clientSocket == INVALID_SOCKET) {
                cout<<"Failed to accept client connection"<<endl;
                continue;
            }


            // Create a new thread to handle the accepted client connection.
            thread clientThread(&ProxyServer::handleClient, this, clientSocket);
            clientThread.detach();
        }
        return true;
    }

    void stop(){
        if(serverSocket != INVALID_SOCKET){
            closesocket(serverSocket);
        }
        WSACleanup();
    }
};

using namespace std;
 
int main()
{
    ProxyServer proxy(8080, 300);

    if(!proxy.start()){
        cout<<"Failed to start the proxy server."<<endl;
    }

    return 0;
}
