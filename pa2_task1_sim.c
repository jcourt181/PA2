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
# Student #3: 

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define TIMEOUT_MS 1000
#define LOSS_RATE 10  // 10% packet loss simulation

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000;

typedef struct {
    int socket_fd;
    long tx_packets;
    long rx_packets;
    long retry_packets;
    long lost_packets;
    long long total_rtt;
} client_thread_data_t;

// Simple random number generator
static int should_drop_packet() {
    static unsigned int seed = 0;
    if (seed == 0) seed = time(NULL);
    return (rand_r(&seed) % 100 < LOSS_RATE);
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    char send_buf[MESSAGE_SIZE];
    char recv_buf[MESSAGE_SIZE];
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    data->tx_packets = 0;
    data->rx_packets = 0;
    data->retry_packets = 0;
    data->lost_packets = 0;
    data->total_rtt = 0;

    for (int seq = 0; seq < num_requests; seq++) {
        struct timeval start, end;
        int attempts = 0;
        int ack_received = 0;
        
        // Prepare packet with sequence number
        snprintf(send_buf, MESSAGE_SIZE, "PKT%12d", seq);

        while (!ack_received && attempts < 3) {
            gettimeofday(&start, NULL);
            
            if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0,
                      (struct sockaddr *)&server_addr, addr_len) < 0) {
                perror("sendto failed");
                break;
            }

            if (attempts == 0) {
                data->tx_packets++;
            } else {
                data->retry_packets++;
            }

            // Setup epoll for timeout
            int epoll_fd = epoll_create1(0);
            struct epoll_event event, events[MAX_EVENTS];
            event.events = EPOLLIN;
            event.data.fd = data->socket_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event);

            int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, TIMEOUT_MS);
            close(epoll_fd);
            
            if (ready > 0) {
                ssize_t recv_len = recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0,
                                           (struct sockaddr *)&server_addr, &addr_len);
                if (recv_len == MESSAGE_SIZE) {
                    // Verify ACK contains our sequence number
                    int ack_seq;
                    if (sscanf(recv_buf, "PKT%12d", &ack_seq) == 1 && ack_seq == seq) {
                        ack_received = 1;
                        data->rx_packets++;
                        gettimeofday(&end, NULL);
                        data->total_rtt += (end.tv_sec - start.tv_sec) * 1000000 + 
                                          (end.tv_usec - start.tv_usec);
                    }
                }
            }
            attempts++;
        }

        if (!ack_received) {
            data->lost_packets++;
        }
    }

    close(data->socket_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        
        // Set socket timeout using setsockopt
        struct timeval tv;
        tv.tv_sec = TIMEOUT_MS / 1000;
        tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
        if (setsockopt(thread_data[i].socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            perror("setsockopt failed");
            exit(EXIT_FAILURE);
        }

        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long total_tx = 0, total_rx = 0, total_retry = 0, total_lost = 0;
    long long total_rtt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx += thread_data[i].tx_packets;
        total_rx += thread_data[i].rx_packets;
        total_retry += thread_data[i].retry_packets;
        total_lost += thread_data[i].lost_packets;
        total_rtt += thread_data[i].total_rtt;
    }

    printf("\n=== Statistics ===\n");\
    printf("Total Packets Lost: %ld\n", total_lost);
    printf("Packet Loss Rate: %.2f%%\n", (total_lost * 100.0) / total_tx);\
    if (total_rx > 0) {
        printf("Average RTT: %.2f us\n", (double)total_rtt / total_rx);
    }
    printf("\n");
}

void run_server() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(server_port);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    printf("Server running on %s:%d (%.1f%% packet loss)\n", server_ip, server_port, (float)LOSS_RATE);

    while (1) {
        char buffer[MESSAGE_SIZE];
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);

        ssize_t n = recvfrom(sockfd, buffer, MESSAGE_SIZE, 0, 
                            (struct sockaddr *)&cliaddr, &len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        // Simulate packet loss
        if (!should_drop_packet()) {
            if (sendto(sockfd, buffer, n, 0, 
                      (struct sockaddr *)&cliaddr, len) < 0) {
                perror("sendto failed");
            }
        }
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