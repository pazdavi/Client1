#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>
#include "protocol.h"

#define PORT 8888
#define MAX_CLIENTS 10
#define GAME_LOBBY_TIME 120
#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 12345
#define ANSWER_TIMEOUT 30
#define KEEPALIVE_TIMEOUT 10

typedef struct {
    int socket;
    struct sockaddr_in addr;
    int verified;
    int score;
    int auth_code;
    time_t last_keepalive;
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
int game_started = 0;

TriviaQuestion questions[6] = {
    {"Capital of France?", {"Berlin", "Madrid", "Paris", "Rome"}, 2},
    {"Red Planet?", {"Earth", "Mars", "Jupiter", "Saturn"}, 1},
    {"5 * 6?", {"11", "30", "56", "20"}, 1},
    {"Project language?", {"Python", "C", "Java", "Rust"}, 1},
    {"Mona Lisa?", {"Da Vinci", "Van Gogh", "Picasso", "Rembrandt"}, 0},
    {"Largest ocean?", {"Atlantic", "Indian", "Pacific", "Arctic"}, 2}
};

void* handle_client(void* arg);
void* game_lobby_timer(void* arg);
void* keepalive_checker(void* arg);
void start_game();

int main() {
    srand(time(NULL));
    int server_fd;
    struct sockaddr_in server_addr;
    pthread_t lobby_thread, keep_thread;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 5);

    printf("Server running on port %d. Waiting for clients...\n", PORT);
    pthread_create(&lobby_thread, NULL, game_lobby_timer, NULL);
    pthread_create(&keep_thread, NULL, keepalive_checker, NULL);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &len);

        if (game_started || client_count >= MAX_CLIENTS) {
            printf("Rejected: game started or max clients.\n");
            close(client_sock);
            continue;
        }

        clients[client_count].socket = client_sock;
        clients[client_count].addr = client_addr;
        clients[client_count].verified = 0;
        clients[client_count].score = 0;
        clients[client_count].last_keepalive = time(NULL);

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, &clients[client_count]);
        client_count++;
    }
    return 0;
}

void* handle_client(void* arg) {
    Client* client = (Client*)arg;
    TrvMessage msg;

    client->auth_code = rand() % 9000 + 1000;
    char code_str[32];
    snprintf(code_str, sizeof(code_str), "%d", client->auth_code);
    build_message(&msg, TRV_AUTH_CODE, 0, code_str);
    send(client->socket, &msg, 4 + msg.payload_len, 0);

    recv(client->socket, &msg, sizeof(msg), 0);
    msg.payload[msg.payload_len] = '\0';

    if (msg.type != TRV_AUTH_REPLY || atoi(msg.payload) != client->auth_code) {
        build_message(&msg, TRV_AUTH_FAIL, 0, "Invalid code.");
        send(client->socket, &msg, 4 + msg.payload_len, 0);
        close(client->socket);
        pthread_exit(NULL);
    }

    client->verified = 1;
    build_message(&msg, TRV_AUTH_OK, 0, "Welcome to the trivia game!");
    send(client->socket, &msg, 4 + msg.payload_len, 0);

    printf("Verified client: %s:%d\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));

    while (1) {
        int n = recv(client->socket, &msg, sizeof(msg), 0);
        if (n <= 0) break;

        if (msg.type == TRV_KEEPALIVE) {
            client->last_keepalive = time(NULL);
        } else if (msg.type == TRV_ANSWER) {
            int qid = msg.question_id;
            int ans = atoi(msg.payload);
            if (qid >= 0 && qid < 6 && ans == questions[qid].correct_index + 1) {
                client->score++;
                printf("Correct answer from client %s:%d\n",
                       inet_ntoa(client->addr.sin_addr),
                       ntohs(client->addr.sin_port));
            }
        }
    }
    close(client->socket);
    pthread_exit(NULL);
}

void* game_lobby_timer(void* arg) {
    printf("Lobby open for %d seconds...\n", GAME_LOBBY_TIME);
    sleep(GAME_LOBBY_TIME);
    game_started = 1;
    printf("Lobby closed. Starting game!\n");
    start_game();
    return NULL;
}

void start_game() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    mcast_addr.sin_port = htons(MULTICAST_PORT);

    TrvMessage msg;

    for (int i = 0; i < 6; i++) {
        char q_text[512];
        snprintf(q_text, sizeof(q_text), "%s\n1. %s\n2. %s\n3. %s\n4. %s",
                 questions[i].question,
                 questions[i].options[0],
                 questions[i].options[1],
                 questions[i].options[2],
                 questions[i].options[3]);

        build_message(&msg, TRV_QUESTION, i, q_text);
        sendto(sockfd, &msg, 4 + msg.payload_len, 0,
               (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

        printf("Sent question %d. Waiting for answers...\n", i + 1);
        sleep(ANSWER_TIMEOUT);
    }

    // Calculate winner
    int max_score = -1, winner_index = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].verified && clients[i].score > max_score) {
            max_score = clients[i].score;
            winner_index = i;
        }
    }

    if (winner_index != -1) {
        char winner_msg[256];
        snprintf(winner_msg, sizeof(winner_msg),
                 "Winner: %s:%d with score %d!",
                 inet_ntoa(clients[winner_index].addr.sin_addr),
                 ntohs(clients[winner_index].addr.sin_port),
                 max_score);
        build_message(&msg, TRV_WINNER, 0, winner_msg);

        for (int i = 0; i < client_count; i++) {
            if (clients[i].verified) {
                send(clients[i].socket, &msg, 4 + msg.payload_len, 0);
                close(clients[i].socket);
            }
        }
    }
    close(sockfd);
    printf("Game ended.\n");
}

void* keepalive_checker(void* arg) {
    while (1) {
        sleep(5);
        time_t now = time(NULL);
        for (int i = 0; i < client_count; i++) {
            if (clients[i].verified && difftime(now, clients[i].last_keepalive) > KEEPALIVE_TIMEOUT) {
                printf("Client %s:%d timed out.\n",
                       inet_ntoa(clients[i].addr.sin_addr),
                       ntohs(clients[i].addr.sin_port));
                close(clients[i].socket);
                clients[i].verified = 0;
            }
        }
    }
    return NULL;
}
