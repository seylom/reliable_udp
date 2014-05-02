#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#include "helper.h"

#define MAXBUFLEN 1500
#define WINDOW_SIZE 10

void reliablyReceive(unsigned short int myUDPport, char* destinationFile);
void sigchld_handler(int s);
int establish_receive_connection();
int establisb_send_connection(char* hostname);
void send_dgram(char *host, char *port, reliable_dgram *dgram);
void process_packet(struct reliable_dgram* dgram);
void write_to_file();
void initialize_window();
void *packet_handler(void *datapv);
int map_seq_to_window(int seq);

struct sockaddr_storage their_addr;
char destination[256];

typedef struct packet_info{
    char* hostname;
    struct reliable_dgram *datagram;
};

typedef struct window_slot{
     int ack;
     int written;
     int received;
     int seq;
     
     char *data;
};

struct window_slot window[WINDOW_SIZE];

/* Global variable storing current port in use */
char port[6];
int socket_back_to_sender = -1;
char send_port[6] ;
struct addrinfo *client_info;
FILE *file;
pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
int next_slot = 0;
int max_window_slots = 5;
int next_expected_packet;

int window_start = 0;
int current_seq = 0;
int next_non_written = 0;

/* Maps the actual sequence number to a index in sliding window */
int map_seq_to_window(int seq) {
	int index = seq % WINDOW_SIZE;
	return index;
}

int window_has_room() {
	if (window_start + WINDOW_SIZE > current_seq)
		return 1;
	return 0;
}

int main(int argc, char** argv) {
	unsigned short int udpPort;

	if (argc != 3) {
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	udpPort = (unsigned short int) atoi(argv[1]);
	sprintf(port, "%d", udpPort);
	sprintf(send_port, "%d", udpPort + 5);
	
	initialize_window();

	reliablyReceive(udpPort, argv[2]);
	
	return 0;
}

/*
*   Initializes receiving window information
*/
void initialize_window(){
    int i = 0;
    for(i=0; i< max_window_slots; i++){
        window[i].ack = 0;
        window[i].written = 0;
        window[i].received = 0;   
        window[i].seq = 0;
    }
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*) sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*) sa)->sin6_addr);
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {

    if (access(destinationFile, F_OK) != -1){
        if (remove(destinationFile) == 0){
            //printf("reliable_receiver: existing destination file removed\n");
        }
    }
    
    file = fopen(destinationFile, "w");
    
    if (file == NULL){
        printf("reliable_receiver: Unable to create the destination file\n");
        exit(1);
    }
    
    fclose(file);
    
    memcpy(destination, destinationFile, strlen(destinationFile));
    destination[strlen(destinationFile)] = 0;
    
	int sockfd = establish_receive_connection();
	
	while (1) {
		receivePacket(sockfd);
	}
}

void receivePacket(int sockfd) {
	int numbytes;
	char buf[MAXBUFLEN];
	char s[INET6_ADDRSTRLEN];
	socklen_t addr_len = sizeof their_addr;

	if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN - 1, 0,
			(struct sockaddr *) &their_addr, &addr_len)) == -1) {
		perror("recvfrom");
		exit(1);
	}
	
	char* sender_host = inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
	
	reliable_dgram *dgram = (reliable_dgram *)buf;
	
	//printf("reliable_receiver: got packet from %s\n", sender_host);
	//printf("reliable_receiver: received dgram with seq #%d, size %d\n", dgram->seq, dgram->size);
	//printf("reliable_receiver: packet is %d bytes long\n", numbytes);
	
	//buf[numbytes] = '\0';
	
	struct packet_info *packet;
	packet = malloc(sizeof(struct packet_info));
	
	packet->hostname = malloc(256);
	memcpy(packet->hostname, sender_host, strlen(sender_host));
	packet->hostname[strlen(sender_host)] = 0;
	
	packet->datagram = malloc(sizeof(*dgram));
	*(packet->datagram) = *dgram;
	
	pthread_t thread;
	pthread_create(&thread, NULL, (void*)packet_handler, (void*)packet);
}

void *packet_handler(void *datapv){
   
    struct packet_info *packet =  (struct packet_info *)datapv;
 
    reliable_dgram *dgram = packet->datagram;
    
	sendAck(packet->hostname, dgram->seq);
	
	dgram->payload[DATA_SIZE] = 0;
	
	process_packet(dgram);
	write_to_file();
	
	//free packet here.
}

void sendAck(char* hostName, int seq) {

    struct reliable_dgram dgram;
    
    dgram.seq = seq;
    dgram.size = 3*sizeof(char);
    memcpy(dgram.payload,"ACK",dgram.size);
    
    printf("reliable_receiver: Sending ACK for seq #%d\n", seq);
    send_dgram(hostName, send_port, &dgram);
}

/*
*   store packet int the window
*/
void process_packet(struct reliable_dgram *dgram){

    pthread_mutex_lock(&window_lock);
    
    //store packet in window
    int slot = map_seq_to_window(dgram->seq);
    
    if (window[slot].received == 0){
        window[slot].received = 1;
        window[slot].ack = 1;
        window[slot].seq = dgram->seq;
        
        window[slot].data = malloc(sizeof(dgram->payload));
        memcpy(window[slot].data, dgram->payload, strlen(dgram->payload));
    }
    
    pthread_mutex_unlock(&window_lock);
}

/*
*Writes the provided data to the destination file
*/
void write_to_file(){

    pthread_mutex_lock(&file_lock);
    
    int i = 0;
    int written_count = 0;
    
    //only write if we have packets in the correct order
    for(i = next_non_written; i < WINDOW_SIZE; i++){
        if (window[i].received == 0)
            break;
            
        if (window[i].received == 1 && window[i].written == 0){
        
            printf("reliable_receiver: writting seq#%d to file\n", window[i].seq);
            
            file = fopen(destination,"a");  
            fputs(window[i].data, file);
            fclose(file);
            
            window[i].received = 0;
            free(window[i].data);
            window[i].data = NULL;
            
            written_count++;
        }
    }
    
    //mark the index in the sliding window of what we need to write next
    next_non_written += written_count;
    
    pthread_mutex_unlock(&file_lock);
}

int establish_receive_connection() {
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // set to AF_INET to force IPv4
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))
				== -1) {
			perror("reliable_receiver: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("reliable_receiver: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		return 2;
	}
	freeaddrinfo(servinfo);

	return sockfd;
}

int establish_send_connection(char* hostname) {
	int sockfd;
	int rv;
	struct addrinfo hints, *servinfo, *p;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(hostname, send_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))
				== -1) {
			perror("reliable_receiver: socket");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "reliable_receiver: failed to bind socket\n");
		return 2;
	}
	
	freeaddrinfo(servinfo);
	
	client_info = malloc(sizeof *p);
	
	*client_info = *p;
	
	return sockfd;
}

/*
*   sends UDP message
*/
void send_dgram(char *host, char *port, reliable_dgram *dgram){

    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    int numbytes;

    //char *message = (char*)dgram;
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and make a socket
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("node: socket");
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "node: failed to bind socket\n");
        return 2;
    }

    if ((numbytes = sendto(sockfd, (char*)dgram, MAXBUFLEN - 1, 0,
             p->ai_addr, p->ai_addrlen)) == -1) {
        perror("node: sendto");
        exit(1);
    }

    freeaddrinfo(servinfo); 
    
    close(sockfd);
}

void sigchld_handler(int s) {
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

