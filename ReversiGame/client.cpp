#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define BUFFER_SIZE 1024

int client_socket;
char buffer[BUFFER_SIZE];

void *receive_messages(void *arg)
{
    bzero(buffer, BUFFER_SIZE);
    while (1)
    {
        bzero(buffer, BUFFER_SIZE);
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received < 0)
        {
            perror("Eroare la primirea răspunsului de la server");
            break;
        }
        else if (bytes_received == 0)
        {
            printf("Serverul s-a deconectat.\n");
            break;
        }

        printf("\033[H\033[J");
        printf("%s\n", buffer);
    }
    return NULL;
}

int main()
{
    struct sockaddr_in server_address;
    char command[BUFFER_SIZE];
    int optval = 1;

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0)
    {
        perror("Eroare la crearea socket-ului client");
        exit(EXIT_FAILURE);
    }

    setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    server_address.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Eroare la conectarea la server");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    printf("Conectat la serverul %s:%d\n", SERVER_IP, SERVER_PORT);
    bzero(buffer, BUFFER_SIZE);
    pthread_t receive_thread;
    pthread_create(&receive_thread, NULL, receive_messages, NULL);

    while (1)
    {
        printf("Utilizati help pentru a vedea comenzile.\n");
        bzero(command, BUFFER_SIZE);
        fgets(command, BUFFER_SIZE, stdin);
        bzero(buffer, BUFFER_SIZE);
        command[strcspn(command, "\n")] = 0;

        if (strcmp(command, "quit") == 0)
        {
            printf("Deconectare...\n");
            break;
        }

        if (send(client_socket, command, strlen(command), 0) < 0)
        {
            perror("Eroare la trimiterea comenzii către server");
            break;
        }
    }

    close(client_socket);
    return 0;
}
