#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <math.h>

#include "utils.h"

struct sent_packet
{
    struct packet pkt;
    int resent;
    struct timeval time_sent;
};

void serve_packet(struct packet *pkt, int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    int bytes_sent = sendto(sockfd, pkt, PACKET_SIZE, 0, (struct sockaddr *)addr, addr_size);
    if (bytes_sent < 0)
    {
        perror("Error sending packet");
        exit(1);
    }
}

void send_handshake(int file_size, struct packet *pkt, int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    // Setting the sequence number to the file size
    pkt->seqnum = file_size;
 
    serve_packet(pkt, sockfd, addr, addr_size);
}

void set_socket_timeout(int sockfd, struct timeval timeout)
{
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        perror("Error setting socket timeout");
        exit(1);
    }
}

int recv_ack(int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    int acknum;
    int bytes_received = recvfrom(sockfd, &acknum, sizeof(acknum), 0, (struct sockaddr *)addr, &addr_size);
    if (bytes_received < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            return -2;
        }
        else
        {
            perror("Recvfrom failed");
            return -1;
        }
    }
   
    return (int)acknum;
}

// Function that reads in from the file and creates a packet with the next contents
int read_file_and_create_packet(FILE *fp, struct packet *pkt, int seq_num)
{
    // Read in the file
    char payload[PAYLOAD_SIZE];
    int bytes_read = fread(payload, 1, PAYLOAD_SIZE, fp);
    if (bytes_read < 0)
    {
        perror("Error reading file");
        exit(1);
    }
    // Build the packet
    build_packet(pkt, seq_num, bytes_read, payload);
    return bytes_read;
}

// Function that will buffer sent packets for us
int buffer_packet(struct packet *pkt, struct sent_packet *buffer, int ack_num)
{
    int ind = pkt->seqnum - ack_num;
    if (ind < 0)
    {
        printf("Already received ACK up to packet %d, which occurs after packet %d\n", ack_num, pkt->seqnum);
        return -1;
    }
    if (ind >= MAX_BUFFER)
    {
        printf("Exceeded maximum window size with packet %d while waiting for ack for %d\n", pkt->seqnum, ack_num);
        return -1;
    }
    buffer[ind].pkt = *pkt;
    buffer[ind].resent = 0;
    gettimeofday(&buffer[ind].time_sent, NULL);
    // printf("Buffered packet %d\n", pkt->seqnum);
    return ind;
}

// Function that handles moving the buffer forward upon receiving an ACK
int handle_ack(struct sent_packet *buffer, int old_ack, int new_ack)
{
    int num_pkt_recv = new_ack - old_ack;
    if (old_ack != buffer[0].pkt.seqnum)
    {
        printf("ERROR: old_ack %d does not match buffer[0].pkt.seqnum %d\n", old_ack, buffer[0].pkt.seqnum);
        exit(1);
    }
    if (num_pkt_recv <= 0)
    {
    
        return old_ack;
    }
    if (num_pkt_recv > MAX_BUFFER)
    {
        perror("Received ACK that should not have been sent yet");
        return old_ack;
    }
    for (int i = num_pkt_recv; i < MAX_BUFFER; i++)
    {
        buffer[i - num_pkt_recv] = buffer[i];
    }
    for (int i = MAX_BUFFER - num_pkt_recv; i < MAX_BUFFER; i++)
    {
        buffer[i].resent = 0;
    }
    
    return new_ack;
}

// Function that marks a packet as resent after resending it
void resend_packet(
    struct sent_packet *buffer,
    int packet_num,
    int ack_num,
    int sockfd,
    struct sockaddr_in *addr,
    socklen_t addr_size)
{
    int ind = packet_num - ack_num;
    if (ind < 0 || ind >= MAX_BUFFER)
    {
        printf("Can't resend packet %d - not currently buffered", packet_num);
        return;
    }
    serve_packet(&buffer[ind].pkt, sockfd, addr, addr_size);
    buffer[ind].resent = 1;
}

void update_est_rtt(struct timeval *est_rtt, struct timeval *dev_rtt, struct timeval *sample_rtt)
{
    // Update the estimated RTT
    est_rtt->tv_sec = ((1.0 - ALPHA) * est_rtt->tv_sec) + (ALPHA * sample_rtt->tv_sec);
    est_rtt->tv_usec = ((1.0 - ALPHA) * est_rtt->tv_usec) + (ALPHA * sample_rtt->tv_usec);
    // Update the deviation RTT
    dev_rtt->tv_sec = ((1.0 - BETA) * dev_rtt->tv_sec) + (BETA * labs(sample_rtt->tv_sec - est_rtt->tv_sec));
    dev_rtt->tv_usec = ((1.0 - BETA) * dev_rtt->tv_usec) + (BETA * labs(sample_rtt->tv_usec - est_rtt->tv_usec));
}

void time_elapsed_since(struct timeval *start, struct timeval *end, struct timeval *elapsed)
{
    elapsed->tv_sec = end->tv_sec - start->tv_sec;
    elapsed->tv_usec = end->tv_usec - start->tv_usec;
    if (elapsed->tv_usec < 0)
    {
        elapsed->tv_sec--;
        elapsed->tv_usec += 1000000;
    }
}

void add_timeval(struct timeval *base_val, struct timeval *to_add)
{
    base_val->tv_sec += to_add->tv_sec;
    base_val->tv_usec += to_add->tv_usec;
    if (base_val->tv_usec >= 1000000)
    {
        base_val->tv_sec++;
        base_val->tv_usec -= 1000000;
    }
}

void send_and_buffer_packet(
    struct packet *pkt,
    struct sent_packet *buffer,
    int ack_num,
    int sockfd,
    struct sockaddr_in *addr,
    socklen_t addr_size)
{
    // Send the packet
    serve_packet(pkt, sockfd, addr, addr_size);
    // Buffer the packet
    buffer_packet(pkt, buffer, ack_num);
}

void send_unsent_packets(
    int cwnd,
    int *seq_num,
    int ack_num,
    FILE *fp,
    struct packet *pkt,
    struct sent_packet *buffer,
    int sockfd,
    struct sockaddr_in *addr,
    socklen_t addr_size)
{
    // The number of currently sent but unacked packets
    int num_unacked = *seq_num - ack_num;
    int num_to_send = cwnd - num_unacked;
    for (int i = 0; i < num_to_send; i++)
    {
        read_file_and_create_packet(fp, pkt, *seq_num);
        (*seq_num)++;
        send_and_buffer_packet(pkt, buffer, ack_num, sockfd, addr, addr_size);
    }
}

int main(int argc, char *argv[])
{
    int listen_sockfd, send_sockfd, new_ack, num_times_ack_repeated, last_ack_cwnd_change, seq_num, ack_num, cwnd, ssthresh;
    struct sockaddr_in client_addr, server_addr_to, server_addr_from;
    socklen_t addr_size = sizeof(server_addr_to);
    struct timeval timeout, dev_rtt;
    struct packet pkt;
    seq_num = 1;
    ack_num = 0;
    num_times_ack_repeated = 0;
    last_ack_cwnd_change = 0;
    cwnd = INITIAL_WINDOW;
    ssthresh = SSTHRESH;
    struct sent_packet buffer[MAX_BUFFER];
    for (int i = 0; i < MAX_BUFFER; i++)
    {
        buffer[i].resent = 0;
    }
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    dev_rtt.tv_sec = 0;
    dev_rtt.tv_usec = 0;

    // read filename from command line argument
    if (argc != 2)
    {
        printf("Usage: ./client <filename>\n");
        return 1;
    }
    char *filename = argv[1];

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    memset(&server_addr_to, 0, sizeof(server_addr_to));
    server_addr_to.sin_family = AF_INET;
    server_addr_to.sin_port = htons(SERVER_PORT_TO);
    server_addr_to.sin_addr.s_addr = inet_addr(SERVER_IP);

    // Configure the client address structure
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the client address
    if (bind(listen_sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Open file for reading
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        perror("Error opening file");
        close(listen_sockfd);
        close(send_sockfd);
        return 1;
    }

    // Get file size
    fseek(fp, 0, SEEK_END); // Move to the end of the file
    int file_size = ftell(fp); // Get the file size
    int num_packets = (int)ceil((double)file_size / PAYLOAD_SIZE); // Calculate the number of packets
    fseek(fp, 0, SEEK_SET); // Move back to the start of the file

    read_file_and_create_packet(fp, &pkt, 0); // Create the initial packet

    // Send handshake
    send_handshake(num_packets, &pkt, send_sockfd, &server_addr_to, addr_size); // Send handshake packet
    set_socket_timeout(listen_sockfd, timeout); // Set socket timeout

    ack_num = recv_ack(listen_sockfd, &server_addr_from, addr_size); // Receive acknowledgment

    while (ack_num != 1) // Resend handshake until acknowledged
    {
        send_handshake(num_packets, &pkt, send_sockfd, &server_addr_to, addr_size);
        ack_num = recv_ack(listen_sockfd, &server_addr_from, addr_size);
    }

    while (ack_num < num_packets) // Send packets until all are acknowledged
    {
        // Additive increase: increase congestion window (cwnd)
        if ((ack_num - last_ack_cwnd_change >= cwnd) || (cwnd <= ssthresh))
        {
            cwnd++;
            last_ack_cwnd_change = ack_num;
        }

        // Limit congestion window size
        cwnd = fmin(cwnd, num_packets - seq_num);
        cwnd = fmin(cwnd, MAX_BUFFER);

        send_unsent_packets(cwnd, &seq_num, ack_num, fp, &pkt, buffer, send_sockfd, &server_addr_to, addr_size); // Send unsent packets

        new_ack = recv_ack(listen_sockfd, &server_addr_from, addr_size); // Receive acknowledgment

        if (new_ack == -1) // Exit on error
        {
            exit(1);
        }
        else if (new_ack == -2) // Handle timeout
        {
            resend_packet(buffer, ack_num, ack_num, send_sockfd, &server_addr_to, addr_size); // Resend packet
            ssthresh = fmax((int)cwnd / 2, 2); // Update slow start threshold
            cwnd = INITIAL_WINDOW; // Reset congestion window
            last_ack_cwnd_change = ack_num;
        }
        else
        {
            if (new_ack == ack_num) // Handle duplicate ACKs
            {
                num_times_ack_repeated++;
                if (num_times_ack_repeated == 3) // Fast retransmit
                {
                    resend_packet(buffer, ack_num, ack_num, send_sockfd, &server_addr_to, addr_size);
                    cwnd /= 2; // Halve the congestion window
                    last_ack_cwnd_change = ack_num;
                    ssthresh = fmax(cwnd, 2); // Update slow start threshold
                    cwnd += 3;
                }
                else if (num_times_ack_repeated > 3) // Fast recovery
                {
                    cwnd++;
                }
            }
            else
            {
                num_times_ack_repeated = 0; // Reset duplicate ACK counter
            }

            ack_num = handle_ack(buffer, ack_num, new_ack); // Update ACK number and buffer
        }
    }
  
    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    return 0;
}
