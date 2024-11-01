#include <iostream>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <mutex>

// Constants for server configuration
const int QUEUE_BACKLOG = 3;                        // Max number of queued connections
const std::string DEFAULT_SERVER_IP = "127.0.0.1";  // Server IP for client-server communication
const int DEFAULT_PORT = 12345;                     // Default port for client-server communication
const int BUFFER_SIZE = 1024;                       // Size of the buffer for message transmission

// Atomic flag for controlling server shutdown on SIGINT
std::atomic<bool> serverRunning(true);  // Tracks server running state
int shutdownSocket = -1;                // Additional socket to trigger select() exit on shutdown

// Atomic counter for assigning unique client IDs
std::atomic<int> clientCounter(0);

// Mutex for thread safety in cleanup with more threads handling clients concurrently
std::mutex clientMutex;

// Signal handler function to intercept SIGINT and update server state
// - Purpose: Allows graceful server shutdown when SIGINT is received (e.g., from Ctrl+C)
// - Complexity: O(1), as it directly sets an atomic flag
void handleSignal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nSIGINT received, shutting down server...\n";
        serverRunning = false;

        // Wake up select() by sending data on the shutdownSocket
        if (shutdownSocket != -1) {
            char shutdownSignal = 1;
            send(shutdownSocket, &shutdownSignal, sizeof(shutdownSignal), 0);
        }
    }
}

// Data Structure Justification:
// Using unordered_map for client management: maps client sockets to client-specific data.
// Average-case complexity: O(1) for insert, delete, and lookup.
// Alternative structures, like std::map (O(log n) complexity), would be slower for large client pools.
class TCPServer {
private:
    int port;                                      // Port on which the server operates
    int serverSocket;                              // Server socket descriptor
    fd_set masterSet;                              // Master set for handling multiple connections
    int maxFd;                                     // Tracks max file descriptor, optimizing select() calls
    std::unordered_map<int, std::string> clientAddresses;  // Maps client socket to IP addresses
    std::unordered_map<int, int> clientIDs;        // Maps client socket to a unique client ID

public:
    explicit TCPServer(int port) : port(port), serverSocket(-1), maxFd(0) {
        FD_ZERO(&masterSet);  // Initialize the master set at server instantiation
        std::signal(SIGINT, handleSignal);  // Register the signal handler
    }

    // Step 1: Initialize and start server socket (Create -> Bind -> Listen)
    bool startServer() {
        return createSocket() && bindSocket() && listenSocket() && createShutdownSocket();
    }

    // Step 1.1: Create the server socket with non-blocking mode for asynchronous handling
    // - Big O Analysis: Creation and setup (O(1)), since it's a constant-time system call.
    bool createSocket() {
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket == -1) {
            std::cerr << "Error creating socket\n";
            return false;
        }
        int flags = fcntl(serverSocket, F_GETFL, 0);
        fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);  // Non-blocking mode enables concurrent client handling
        return true;
    }

    // Step 1.2: Bind socket to IP and port
    // - Big O Analysis: Binding a socket is O(1) as it is a one-time setup operation.
    bool bindSocket() {
        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_addr.s_addr = INADDR_ANY;        // Accept connections on any IP assigned to the server
        serverAddress.sin_port = htons(port);              // Network byte order for consistency across systems

        if (bind(serverSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) < 0) {
            std::cerr << "Binding failed\n";
            return false;
        }
        return true;
    }

    // Step 1.3: Listen for incoming connections
    // - Big O Analysis: Setting a socket to listen is also O(1).
    bool listenSocket() {
        if (listen(serverSocket, QUEUE_BACKLOG) < 0) {
            std::cerr << "Listening failed\n";
            return false;
        }
        FD_SET(serverSocket, &masterSet);  // Include server socket in the master set
        maxFd = serverSocket;  // Initialize maxFd to optimize the select() call
        std::cout << "Server initialized, waiting for connections...\n";
        return true;
    }

    // Step 1.4: Create a shutdown socket to force select() return on shutdown
    bool createShutdownSocket() {
        shutdownSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (shutdownSocket == -1) {
            std::cerr << "Error creating shutdown socket\n";
            return false;
        }
        FD_SET(shutdownSocket, &masterSet);
        if (shutdownSocket > maxFd) {
            maxFd = shutdownSocket;
        }
        return true;
    }

    // Step 2: Main server loop to handle multiple client connections asynchronously
    void run() {
        while (serverRunning) { // Graceful shutdown upon SIGINT
            fd_set readSet = masterSet;  // Duplicate master set for use in select()
            if (select(maxFd + 1, &readSet, nullptr, nullptr, nullptr) < 0) {
                if (serverRunning) {
                    std::cerr << "Select error\n";
                }
                break;
            }

            for (int i = 0; i <= maxFd; ++i) {
                if (FD_ISSET(i, &readSet)) {
                    if (i == serverSocket) {
                        acceptConnection();  // Accept a new client connection
                    } else if (i == shutdownSocket) {
                        close(shutdownSocket);
                        FD_CLR(shutdownSocket, &masterSet);
                    } else {
                        handleClient(i);  // Handle existing client message exchange
                    }
                }
            }
        }

        cleanup();  // Cleanup resources after server stops
    }

    // Step 2.1: Accept a client connection
    // - Complexity Analysis: acceptConnection() is O(1) as it processes a single connection at a time.
    // - Scalability: select() is less efficient with thousands of connections, consider poll or epoll in larger systems.
    void acceptConnection() {
        sockaddr_in clientAddress{};
        socklen_t clientAddressLen = sizeof(clientAddress);
        int clientSocket = accept(serverSocket, (struct sockaddr *) &clientAddress, &clientAddressLen);
        if (clientSocket < 0) {
            std::cerr << "Failed to accept connection\n";
            return;
        }

        // Non-blocking mode for client socket allows asynchronous handling
        int flags = fcntl(clientSocket, F_GETFL, 0);
        fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);
        FD_SET(clientSocket, &masterSet);  // Add client to master set
        if (clientSocket > maxFd) {
            maxFd = clientSocket;  // Update maxFd for select() efficiency
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddress.sin_addr, clientIP, INET_ADDRSTRLEN);
        clientAddresses[clientSocket] = std::string(clientIP) + ":" + std::to_string(ntohs(clientAddress.sin_port));

        // Assign unique ID to the client
        clientIDs[clientSocket] = ++clientCounter;

        std::cout << "TCPClient " << clientIDs[clientSocket] << " connected from " << clientAddresses[clientSocket]
                  << "\n";
    }

    // Step 3: Handle client by receiving message and sending response
    void handleClient(int clientSocket) {
        char buffer[BUFFER_SIZE] = {0};  // Buffer for incoming message

        // Step 3.1: Receive client message
        ssize_t bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
        if (bytesReceived <= 0) {
            if (bytesReceived == 0) {
                std::cout << "TCPClient " << clientIDs[clientSocket] << " disconnected\n";
            } else {
                std::cerr << "Recv error from client " << clientIDs[clientSocket] << "\n";
            }
            close(clientSocket);  // Close client connection
            FD_CLR(clientSocket, &masterSet);  // Remove from master set
            clientAddresses.erase(clientSocket);  // Remove from address map
            clientIDs.erase(clientSocket);  // Remove from client ID map
            return;
        }

        // Print the received message and send a reply
        std::cout << "Received from client " << clientIDs[clientSocket] << ": " << buffer << "\n";

        std::string response = "Message received";
        if (send(clientSocket, response.c_str(), response.size(), 0) < 0) {
            std::cerr << "Send error to client " << clientIDs[clientSocket] << "\n";
        }
    }

    // Step 4: Cleanup all resources after shutdown
    void cleanup() {
        std::lock_guard<std::mutex> lock(clientMutex);  // Ensure thread safety during cleanup
        for (int i = 0; i <= maxFd; ++i) {
            if (FD_ISSET(i, &masterSet)) {
                close(i);  // Close each open file descriptor
            }
        }
        std::cout << "Server cleanup complete.\n";
    }
};

class TCPClient {
private:
    int serverPort;
    std::string serverIP;
    int clientSocket;

public:
    TCPClient(std::string ip, int port) : serverIP(std::move(ip)), serverPort(port), clientSocket(-1) {}

    // Step 1: Initialize the client connection to the server
    bool initializeClient() {
        return createSocket() && connectToServer();
    }

    // Step 1.1: Create the client socket
    bool createSocket() {
        clientSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (clientSocket == -1) {
            std::cerr << "Error creating socket\n";
            return false;
        }
        return true;
    }

    // Step 1.2: Connect to the server
    bool connectToServer() {
        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(serverPort);

        if (inet_pton(AF_INET, serverIP.c_str(), &serverAddress.sin_addr) <= 0) {
            std::cerr << "Invalid address or address not supported\n";
            return false;
        }

        if (connect(clientSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) < 0) {
            std::cerr << "Connection to server failed\n";
            return false;
        }

        std::cout << "Connected to the server!\n";
        return true;
    }

    // Step 2: Send message to the server and await response
    void sendMessage(const std::string &message) {
        if (send(clientSocket, message.c_str(), message.size(), 0) < 0) {
            std::cerr << "Error sending message\n";
            return;
        }

        char buffer[BUFFER_SIZE] = {0};
        ssize_t bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
        if (bytesReceived > 0) {
            std::cout << "Server response: " << buffer << "\n";
        } else {
            std::cerr << "Error receiving server response\n";
        }
    }

    ~TCPClient() {
        if (clientSocket != -1) {
            close(clientSocket);
        }
    }
};

// Main function to demonstrate multi-client connection and server-client interaction
int main() {
    TCPServer server(DEFAULT_PORT);

    if (server.startServer()) {
        std::thread serverThread([&]() { server.run(); });

        TCPClient client1(DEFAULT_SERVER_IP, DEFAULT_PORT);
        client1.initializeClient();
        client1.sendMessage("Hello from client 1!");

        TCPClient client2(DEFAULT_SERVER_IP, DEFAULT_PORT);
        client2.initializeClient();
        client2.sendMessage("Hello from client 2!");

        serverThread.join();
    } else {
        std::cerr << "Failed to start server\n";
    }

    return 0;
}
