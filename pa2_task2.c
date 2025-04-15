/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: John Courtney
# Student #2: Richard Zhang

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 12
#define DEFAULT_CLIENT_THREADS 4
#define MAX_RETRIES 10  // Increased for reliable delivery
#define RTO_US 200000   // 200ms timeout
#define LOSS_RATE 10    // 10% simulated packet loss

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000;

typedef struct {
    uint32_t seq_num;
    uint32_t client_id;  // Added for Task 2
    char payload[MESSAGE_SIZE - 8]; // Adjusted for header
} message_t;

typedef struct {
    int socket_fd;
    uint32_t client_id;  // Unique per thread
    struct sockaddr_in server_addr;
    long tx_cnt;        // First transmissions only
    long rx_cnt;        // Successful ACKs
    long retry_cnt;     // All retransmissions
    long long total_rtt;
} client_thread_data_t;

// Random packet loss simulation
int should_drop_packet() {
    static unsigned int seed = 0;
    if (seed == 0) seed = time(NULL);
    return (rand_r(&seed) % 100) < LOSS_RATE;
}

void error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int wait_for_ack(int socket_fd, uint32_t seq_num, uint32_t client_id, 
                struct sockaddr_in *server_addr, socklen_t addr_len) {
    struct timeval tv;
    fd_set readfds;
    message_t recv_msg;

    tv.tv_sec = RTO_US / 1000000;
    tv.tv_usec = RTO_US % 1000000;

    FD_ZERO(&readfds);
    FD_SET(socket_fd, &readfds);

    int ready = select(socket_fd + 1, &readfds, NULL, NULL, &tv);
    if (ready == -1) error("select error");
    if (ready == 0) return 0; // Timeout

    ssize_t recv_len = recvfrom(socket_fd, &recv_msg, sizeof(message_t), 0,
                               (struct sockaddr *)server_addr, &addr_len);
    if (recv_len < 0) error("recvfrom error");

    // Verify both sequence number and client ID
    return (ntohl(recv_msg.seq_num) == seq_num && 
            ntohl(recv_msg.client_id) == client_id);
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    message_t send_msg, recv_msg;
    socklen_t addr_len = sizeof(data->server_addr);

    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->retry_cnt = 0;
    data->total_rtt = 0;

    for (uint32_t seq = 0; seq < num_requests; seq++) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Prepare message
        send_msg.seq_num = htonl(seq);
        send_msg.client_id = htonl(data->client_id);
        memset(send_msg.payload, 'A' + (seq % 26), sizeof(send_msg.payload));

        data->tx_cnt++;  // Count first transmission

        int attempts = 0;
        while (1) {  // Retry until success
            if (sendto(data->socket_fd, &send_msg, sizeof(message_t), 0,
                      (struct sockaddr *)&data->server_addr, addr_len) < 0) {
                error("sendto failed");
            }

            if (wait_for_ack(data->socket_fd, seq, data->client_id, 
                           &data->server_addr, addr_len)) {
                data->rx_cnt++;
                gettimeofday(&end, NULL);
                data->total_rtt += (end.tv_sec - start.tv_sec) * 1000000 + 
                                  (end.tv_usec - start.tv_usec);
                break;
            }

            if (++attempts > MAX_RETRIES) {
                fprintf(stderr, "Warning: Exceeded max retries for seq=%u\n", seq);
                continue;  // Keep trying (don't exit for Task 2)
            }
            data->retry_cnt++;
        }
    }
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) error("socket failed");
        
        thread_data[i].client_id = i;  // Unique ID per thread
        memcpy(&thread_data[i].server_addr, &server_addr, sizeof(server_addr));
        
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long total_tx = 0, total_rx = 0, total_retry = 0;
    long long total_rtt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_retry += thread_data[i].retry_cnt;
        total_rtt += thread_data[i].total_rtt;
        close(thread_data[i].socket_fd);
    }

    printf("First Transmissions (tx_cnt): %ld\n", total_tx);
    printf("Successful ACKs (rx_cnt): %ld\n", total_rx);
    printf("Retransmissions: %ld\n", total_retry);
    printf("Packet Delivery Ratio: %.2f%%\n", (total_rx * 100.0) / total_tx);
    printf("Average RTT: %.2f messages/s\n", total_rx ? (double)total_rtt / total_rx : 0);
    printf("No Packet Loss: %s\n", (total_tx == total_rx) ? "SUCCESS" : "FAILURE");
}

void run_server() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) error("socket failed");

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(server_port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        error("bind failed");

    printf("Server running with %d%% simulated loss\n", LOSS_RATE);

    while (1) {
        message_t msg, ack;
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);

        if (recvfrom(sockfd, &msg, sizeof(message_t), 0,
                    (struct sockaddr *)&cliaddr, &len) < 0)
            error("recvfrom failed");

        // Simulate packet loss
        if (should_drop_packet()) continue;

        // Prepare ACK (echo client_id + seq_num)
        ack.seq_num = msg.seq_num;
        ack.client_id = msg.client_id;
        
        if (sendto(sockfd, &ack, sizeof(message_t), 0,
                  (struct sockaddr *)&cliaddr, len) < 0)
            error("sendto failed");
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);
        run_client();
    } else {
        printf("Usage: %s <server|client> [ip] [port] [threads] [requests]\n", argv[0]);
        return EXIT_FAILURE;
    }
    return 0;
}