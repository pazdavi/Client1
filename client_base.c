#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 9999

int tcp_sock;
struct sockaddr_in server_addr;

// Thread function declarations
void* udp_listener_thread(void* arg);
void* keep_alive_thread(void* arg);

int main() {
    char buffer[1024];
    pthread_t udp_thread, keepalive_thread;

    // Ask user before connecting
    char choice[8];
    printf("Do you want to join the game? (y/n): ");
    fgets(choice, sizeof(choice), stdin);

    if (choice[0] != 'y' && choice[0] != 'Y') {
        printf("Canceled. Exiting.\n");
        return 0;
    }

    // Create TCP connection to server
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    memset(&(server_addr.sin_zero), 0, 8);

    if (connect(tcp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connection failed");
        return 1;
    }

    // Receive verification code from server
    memset(buffer, 0, sizeof(buffer));
    recv(tcp_sock, buffer, sizeof(buffer), 0);
    printf("Server: %s", buffer);

    // User inputs the code
    printf("Enter code: ");
    fgets(buffer, sizeof(buffer), stdin);
    send(tcp_sock, buffer, strlen(buffer), 0);

    // Receive verification result
    memset(buffer, 0, sizeof(buffer));
    recv(tcp_sock, buffer, sizeof(buffer), 0);
    printf("Server: %s", buffer);

    if (strncmp(buffer, "Authentication successful", 25) != 0) {
        printf("Exiting.\n");
        close(tcp_sock);
        return 1;
    }

    // Start background threads: UDP listener + TCP Keep-Alive sender
    pthread_create(&udp_thread, NULL, udp_listener_thread, NULL);
    pthread_create(&keepalive_thread, NULL, keep_alive_thread, NULL);

    // Main thread waits for UDP thread to finish
    pthread_join(udp_thread, NULL);
    pthread_cancel(keepalive_thread);
    close(tcp_sock);
    return 0;
}

// Thread that listens to multicast UDP questions
void* udp_listener_thread(void* arg) {
    int udp_sock;
    struct sockaddr_in mcast_addr;
    struct ip_mreq mreq;
    char buffer[1024];

    // Create UDP socket
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind to multicast port
    bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    // Join multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

   while (1) {
    memset(buffer, 0, sizeof(buffer));
    recvfrom(udp_sock, buffer, sizeof(buffer), 0, NULL, NULL);

    printf("\n📨 Question received:\n%s\n", buffer);

    // Send ACK to server via TCP
    send(tcp_sock, "ACK\n", 4, 0);

    printf("Your answer (A/B/C/D), 30 sec timeout: ");
    fflush(stdout);

    // Set up select() on stdin with 30 sec timeout
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;

    int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
    if (ret > 0) {
        fgets(buffer, sizeof(buffer), stdin);
        send(tcp_sock, buffer, strlen(buffer), 0);
    } else if (ret == 0) {
        printf("\n⏰ Time expired. No answer sent.\n");
        send(tcp_sock, "NOANSWER\n", 9, 0);  // optional
    } else {
        perror("select failed");
    }
}


    return NULL;
}

// Thread that sends TCP keep-alive messages every 10 seconds
void* keep_alive_thread(void* arg) {
    while (1) {
        sleep(10);
        send(tcp_sock, "KEEP\n", 5, 0);
    }
    return NULL;
}
