#include <iostream>
#include <string>
#include <cstring>      // For memset
#include <unistd.h>     // For close
#include <sys/socket.h> // For socket APIs
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntoa
#include <thread>

using namespace std;

// A dedicated function to handle all communication with a client
void handle_client(int client_socket, const string& client_ip, int remote_port) {
    cout << "Connection accepted from " << client_ip << ":" << remote_port << "\n";

    const int BUFF_LENGTH = 1024;
    char buffer[BUFF_LENGTH] = {0};

    while (true) {
        memset(buffer, 0, sizeof(buffer)); // Clear the buffer
        int bytes_received = recv(client_socket, buffer, BUFF_LENGTH - 1, 0);

        if (bytes_received < 0) {
            perror("Failed to receive data");
            break; // Exit loop on error
        }

        if (bytes_received == 0) {
            cout << "Client at " << client_ip << ":" << remote_port << " disconnected.\n";
            break; // Exit loop on disconnection
        }
        
        buffer[bytes_received] = '\0'; // Null-terminate the received data
        cout << "Received " << bytes_received << " bytes. Message: \"" << buffer << "\"\n";

                    // Phase 1 basic response
        // Send a response back to the client
        // string response = "We have received your message.\n";
        // int bytes_sent = send(client_socket, response.c_str(), response.length(), 0);
        // if (bytes_sent < 0) {
        //     perror("Failed to send data");
        //     break; // Exit loop on error
        // }
                    // Phase 2 Web server : As a Web server it should respond with a valid HTTP response
        string http_response = 
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 13\r\n"
                                "\r\n"
                                "Hello, World!";

        int bytes_sent = send(client_socket, http_response.c_str(), http_response.length(), 0);
        if (bytes_sent < 0) {
            perror("Failed to send data");
            break; // Exit loop on error
        }
        cout << "Sent " << bytes_sent << " bytes back to the client at " << client_ip << ":" << remote_port << "\n";
    }

    // Close the client socket
    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);
}

void notify_user_about_port(char** argv) {
    cout << "You need to specify a port!\n";
    cout << "Usage: " << argv << " {desired port number}\n";
}

int main(int argc, char **argv) { 
    if (argc < 2) {
        notify_user_about_port(argv);
        return 1;
    }
    int port = atoi(argv[1]); 
    int MAX_CONNECTIONS = 10; // A limit for the number of simultaneous connections
    // 1. Create a socket
    int listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_socket < 0) {
        perror("Failed to create socket");
        return 1;
    }

    // 2. Bind the socket to an IP and port
    sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY; // Listen on any available interface

    if (bind(listening_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("Failed to bind");
        return 1;
    }

    // 3. Listen for connections
    if (listen(listening_socket, MAX_CONNECTIONS) < 0) {
        perror("Failed to listen");
        return 1;
    }
    cout << "Server is listening on port " << port << "...\n";

    // 4. Accept connections in a loop
    while (true) {
        sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);
        
        int client_socket = accept(listening_socket, (struct sockaddr *)&client_address, &client_address_length);
        if (client_socket < 0) {
            perror("Failed to accept connection");
            continue; // Continue to the next iteration to wait for a new connection
        }

        // Handle the new connection in our dedicated function
        // handle_client(client_socket, inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
        thread client_thread(handle_client, client_socket, inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
        cout << "Spawned a new thread to handle the client at " << inet_ntoa(client_address.sin_addr) << ":" << ntohs(client_address.sin_port) << "\n";
        client_thread.detach(); // Detach the thread to allow it to run independently
    }
    
    // The server will run indefinitely, so this part is not reached in normal operation
    close(listening_socket);
    return 0;
}