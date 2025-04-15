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

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("ERROR: epoll_ctl");
        pthread_exit(NULL);
    }

    data->request_rate = 0.0;
    data->total_messages = 0;
    data->total_rtt = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;

    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);
        data->tx_cnt++;

        if (sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0,
                  (struct sockaddr *)&server_addr, addr_len) == -1) {
            perror("ERROR: sendto");
            pthread_exit(NULL);
        }

        int epoll_wait_result = epoll_wait(data->epoll_fd, events, MAX_EVENTS, TIMEOUT_MS);
        
        if (epoll_wait_result == -1) {
            perror("ERROR: epoll_wait");
            pthread_exit(NULL);
        } else if (epoll_wait_result == 0) {
            continue; // Timeout, packet considered lost
        }

        if (recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0,
                   (struct sockaddr *)&server_addr, &addr_len) == -1) {
            perror("ERROR: recvfrom");
            pthread_exit(NULL);
        }

        data->rx_cnt++;
        gettimeofday(&end, NULL);
        data->total_messages++;
        data->total_rtt += (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    }

    data->request_rate = data->total_messages * (1000000.0f / data->total_rtt);

    close(data->socket_fd);
    close(data->epoll_fd);

    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd == -1) {
            perror("ERROR: socket");
            exit(EXIT_FAILURE);
        }

        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            perror("ERROR: epoll_create1");
            exit(EXIT_FAILURE);
        }

        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0;
    long long lost_pkt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);

        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        lost_pkt += thread_data[i].tx_cnt - thread_data[i].rx_cnt;

        close(thread_data[i].epoll_fd);
    }

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Packets Lost: %lld messages\n", lost_pkt);
}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd == -1) {
        perror("ERROR: socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("ERROR: bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        char recv_buf[MESSAGE_SIZE];
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ssize_t recv_len = recvfrom(server_fd, recv_buf, MESSAGE_SIZE, 0,
                                  (struct sockaddr *)&client_addr, &addr_len);
        if (recv_len == -1) {
            perror("ERROR: recvfrom");
            continue;
        }

        if (sendto(server_fd, recv_buf, recv_len, 0,
                  (struct sockaddr *)&client_addr, addr_len) == -1) {
            perror("ERROR: sendto");
        }
    }

    close(server_fd);
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
        printf("Usage: %s <server|client> [server_ip] [server_port] [num_client_threads] [num_requests]\n", argv[0]);
        return EXIT_FAILURE;
    }
    return 0;
}