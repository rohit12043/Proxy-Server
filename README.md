# HTTP Proxy Server

## Overview
This project implements a multi-threaded HTTP proxy server with caching capabilities in C++ using Winsock2 for Windows. It supports HTTP/1.0 and HTTP/1.1, forwarding requests from clients to target servers, and caching GET responses to improve performance. A Least Recently Used (LRU) cache with configurable time-to-live (TTL) values is used for efficient storage and eviction.

## Features
*   Supports HTTP GET, HEAD, and other methods (CONNECT is explicitly rejected).
*   Caches responses for GET requests with configurable TTL (default: 5 minutes).
*   Implements LRU cache eviction when the cache exceeds the maximum size (5 entries by default).
*   Multi-threaded client handling for concurrent connections.
*   Periodic cache statistics logging every 30 seconds.
*   Configurable port and default cache TTL.

## Requirements
*   Windows operating system (due to Winsock2 dependency).
*   C++ compiler with C++11 or later support (e.g., MSVC, MinGW).
*   Winsock2 library (`ws2_32.lib`).

## Building the Project
1.  **Clone the repository:**
    ```bash
    git clone https://github.com/your-username/http-proxy-server.git
    cd http-proxy-server
    ```
2.  **Compile the code**, linking against the Winsock2 library.
    *   **With MinGW:**
        ```bash
        g++ -o proxy proxy.cpp -lws2_32
        ```
    *   **With MSVC:**
        ```bash
        cl proxy.cpp /link ws2_32.lib
        ```

## Usage
1.  **Run the compiled executable:**
    ```bash
    ./proxy
    ```
    (This starts the server on default port 8080 with a default cache TTL of 300 seconds.)
    To specify a custom port or cache TTL, modify the `main` function in `proxy.cpp` directly (e.g., `ProxyServer proxy(9090, 600);`).

2.  **Configure your browser or HTTP client** to use `localhost` as the HTTP proxy and `8080` (or your custom port) as the port.

3.  **Send HTTP requests** through the proxy. The server will:
    *   Forward requests to the target server.
    *   Cache responses for GET requests if cacheable.
    *   Serve cached responses for subsequent identical requests if valid.
    *   Log cache statistics every 30 seconds.

### Sample Usage
*   **Running the Proxy Server:**
    ```
    ./proxy
    ```
    Expected output:
    ```
    Proxy server listening on port 8080
    Default cache time: 300 seconds
    ```
*   **Sending a Request (using curl):**
    ```bash
    curl -x http://localhost:8080 http://example.com
    ```
    Server output (example for a cache miss and then a hit):
    ```
    Handling new client connection.
    Request: GET / HTTP/1.1
    Host: example.com
    Cache MISS for: GET:example.com/
    Forwarded request to the target server
    Cached response for: GET:example.com/ (TTL: 300s)

    # Subsequent identical requests will show:
    Cache HIT for: GET:example.com/
    served from cache
    ```
*   **Cache Statistics:**
    ```
    Cache contains 5 entries
    Valid entries: 4
    ```

## Code Structure (Key Components)
*   **`ProxyServer` Class:** Manages Winsock2 initialization, socket creation, client handling, and the LRU cache. It handles HTTP request/response parsing, cacheability determination, and connection to target servers.
*   **`cacheContainer` struct:** Stores cached response data, timestamp, and maximum age.
*   **`HttpReq` and `HttpResp` structs:** Represent parsed HTTP requests and responses, extracting relevant headers and determining cacheability.

## Limitations
*   Only supports HTTP (HTTPS not supported, as CONNECT method is rejected).
*   Designed for Windows due to Winsock2 dependency.
*   Cache size is fixed at 5 entries (configurable in code).
*   No support for chunked transfer encoding or advanced HTTP/2 features.

## Contributing
Feel free to fork the repository, create a new branch for your feature or bug fix, and submit a pull request.

## License
This project is licensed under the MIT License. See the LICENSE file for details.
