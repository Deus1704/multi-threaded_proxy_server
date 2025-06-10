#include <bits/stdc++.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
using namespace std;

void notify_user_about_port(char** argv){
    cout<<"You need to specify a port!\n";
    cout<<"Usage: " << argv[0] << " {desired port number} \n";
}

int main (int argc, char **argv){ 
    // argc => count of args  
    // argv => individual arguments { name, port , etc}
    
    // validate if the ports are mentioned in the argument;
    if (argc<2) { notify_user_about_port(argv); return 0;}
    int port =  atoi(argv[1]); 

    // 1] Socket
    int basic_socket = socket(AF_INET, SOCK_STREAM, 0); // Default 0 => FOR TCP 

    // 2] Bind the Socket
    struct sockaddr_in address;
    // Never Forget to clear the structure
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port); // TO fix the issue of endian flipping

    int bind_value = bind(basic_socket, (struct sockaddr *)&address, sizeof(address));
    if (bind_value < 0){
        perror("Failed to bind \n");
        return 1;
    }
    // 3] Listen
    int listen_value = listen(basic_socket , 1);
    if (listen_value < 0){
        perror("Failed to listen \n");
        return 1;
    }
    // 4] Accept

    struct sockaddr_in remote_address;
    socklen_t remote_address_length = sizeof(address);

    cout<<"Waiting for new connections ... \n";
    int client_socket = accept(basic_socket, (struct sockaddr *)&remote_address, &remote_address_length);
    if (client_socket < 0){
        perror("Failed to accept \n");
        return 1;
    }
    string client_ip = inet_ntoa(remote_address.sin_addr);
    int remote_port = ntohs(remote_address.sin_port);
    cout<<"Connection accepted from " << client_ip << ":" << remote_port << "\n";

    // Before Recieving define a buffer
    int BUFF_LENGTH = 1024;
    char buffer[BUFF_LENGTH] = {0};

    // 5] Recieve
    while (1){
        memset(buffer, 0, sizeof(buffer)); // Clear the buffer before using it
        int bytes_received = recv(client_socket, buffer, BUFF_LENGTH-1, 0);
        if (bytes_received < 0){
            perror("Failed to receive data \n");
            return 1;
        }
        if (bytes_received == 0){
            cout<<"Client at " << client_ip << ":" << remote_port << " disconnected. \n";
            break;
        }
        
        // If still in the loop, it means we have received some data

        buffer[bytes_received] = '\0'; // Null-terminate the received data
        if (buffer[bytes_received-1] == '\n'){
            buffer[bytes_received-1] = '\0'; // Remove the newline character if present
        }
        cout<<"Received " << bytes_received << " bytes, Message reads : \"" << buffer << "\"" << "\n";

        // Send an Acknowledgement back to the client
        string response = "Hello client at " + client_ip + ":" + to_string(remote_port) + ", We have received your message.\n";
        int bytes_sent =  send(client_socket, response.c_str(), response.length(), 0);
        if (bytes_sent < 0){
            perror("Failed to send data \n");
            return 1;
        }
        cout<<"Sent " << bytes_sent << " bytes back to the client. \n";
    }
    
    // If we reach here, it means the client has disconnected
    cout<<"Shutting down the server. \n";
    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);
    close(basic_socket);

}