#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

struct packet_recv
{
    struct packet pkt;
    int received;
};

int write_packet_to_file(FILE *fp, struct packet *pkt)
{
    size_t bytes_written = fwrite(pkt->payload, 1, pkt->length, fp);
    if (bytes_written < 0)
    {
        perror("Error writing to file");
        exit(1);
    }
    
    
    return bytes_written;
}

void recv_packet(struct packet *pkt, int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    int bytes_received = recvfrom(sockfd, pkt, PACKET_SIZE, 0, (struct sockaddr *)addr, &addr_size);
    if (bytes_received < 0)
    {
        perror("Error receiving packet");
        exit(1);
    }
   
}

int handle_handshake(FILE *fp, struct packet *pkt, int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    recv_packet(pkt, sockfd, addr, addr_size);
    write_packet_to_file(fp, pkt);
    int num_packets_expected = pkt->seqnum;
    return num_packets_expected;
}


void send_ack(int acknum, int sockfd, struct sockaddr_in *addr, socklen_t addr_size)
{
    int bytes_sent = sendto(sockfd, &acknum, sizeof(acknum), 0, (struct sockaddr *)addr, addr_size);
    if (bytes_sent < 0)
    {
        perror("Error sending ACK");
        exit(1);
    }
  
}

// buffers the packet - returns the index the packet was buffered at or -1 if packet was discarded
int buffer_packet(struct packet *pkt, struct packet_recv *buffer, int *expected_seq_num)
{
    int ind = pkt->seqnum - *expected_seq_num;
    if (ind < 0)
    {
      
        return -1;
    }
    if (ind >= MAX_BUFFER)
    {
        //too far ahead
        printf("Packet %d too far ahead, ignoring\n", pkt->seqnum);
        return -1;
    }
    // if receivedf dont buffer
    if (!buffer[ind].received)
    {
        buffer[ind].pkt = *pkt;
        buffer[ind].received = 1;
    }
    return ind;
}

//writes all sequential received packets and updates the expected buffer
void save_packets(FILE *fp, struct packet_recv *buffer, int *expected_seq_num)
{
    int ind = 0;
    while ((ind < MAX_BUFFER) && (buffer[ind].received))
    {
        write_packet_to_file(fp, &buffer[ind].pkt);
        ind++;
        (*expected_seq_num)++;
    }
    // If index is 0 no packets were saved
    if (ind == 0)
    {
        return;
    }
    // Moving the bufferd packets forward appropriately
    for (int i = ind; i < MAX_BUFFER; i++)
    {
        buffer[i - ind] = buffer[i];
    }
    // Marking the packets at the very end as not received
    for (int i = MAX_BUFFER - ind; i < MAX_BUFFER; i++)
    {
        buffer[i].received = 0;
    }
}

int main()
{
    int listen_sockfd, send_sockfd;
    struct sockaddr_in server_addr, client_addr_from, client_addr_to;
    struct packet pkt;
    socklen_t addr_size = sizeof(client_addr_from);
    int expected_seq_num = 1;
    // Initializing a buffer of packets to store out of order packets
    struct packet_recv buffer[MAX_BUFFER];
    int buffered_ind;
    for (int i = 0; i < MAX_BUFFER; i++)
    {
        buffer[i].received = 0;
    }

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Configure the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the server address
    if (bind(listen_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Configure the client address structure
    memset(&client_addr_to, 0, sizeof(client_addr_to));
    client_addr_to.sin_family = AF_INET;
    client_addr_to.sin_addr.s_addr = inet_addr(LOCAL_HOST);
    client_addr_to.sin_port = htons(CLIENT_PORT_TO);

    // Open the target file for writing (always write to output.txt)
    FILE *fp = fopen("output.txt", "wb");

    // TODO: Receive file from the client and save it as output.txt
   
    int num_packets = handle_handshake(fp, &pkt, listen_sockfd, &client_addr_from, addr_size);
    send_ack(expected_seq_num, send_sockfd, &client_addr_to, addr_size);
   
    // Dealing with repeated handshake messages
    while (pkt.seqnum == num_packets)
    {
       
        recv_packet(&pkt, listen_sockfd, &client_addr_from, addr_size);
        send_ack(expected_seq_num, send_sockfd, &client_addr_to, addr_size);
    }
   
    while (expected_seq_num < num_packets)
    {
        recv_packet(&pkt, listen_sockfd, &client_addr_from, addr_size);
      
        buffered_ind = buffer_packet(&pkt, buffer, &expected_seq_num);
        if (buffered_ind > -1)
        {
            save_packets(fp, buffer, &expected_seq_num);
        }
        send_ack(expected_seq_num, send_sockfd, &client_addr_to, addr_size);
    }
   
    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    return 0;
}
