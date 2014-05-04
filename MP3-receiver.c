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
#define WINDOW_SIZE 200

typedef struct packet_info{
    char* hostname;
    struct reliable_dgram *datagram;
};

void reliablyReceive(unsigned short int myUDPport, char* destinationFile);
void sigchld_handler(int s);
int establish_receive_connection();
int establisb_send_connection(char* hostname);
void send_dgram(char *host, char *port, reliable_dgram *dgram);
void process_packet(struct packet_info *packet);
void write_to_file();
void initialize_window();
void *packet_handler(void *datapv);
int map_seq_to_window(int seq);
void *write_handler(void *datapv);

struct sockaddr_storage their_addr;
char destination[256];

typedef struct window_slot{
     int ack;
     int written;
     int received;
     int seq;
     
     char *data;
	 char *hostname;
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
int available_slots = 0;
int next_expected_packet;

volatile int window_start = 0;
int current_seq = 0;
int next_non_written = 0;

/* Maps the actual sequence number to a index in sliding window */
int map_seq_to_window(int seq) {
	int index = seq % WINDOW_SIZE;
	return index;
}

int window_has_room() {

	if (available_slots > 0)
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
    for(i=0; i< WINDOW_SIZE; i++){
        window[i].ack = 0;
        window[i].written = 0;
        window[i].received = 0;   
        window[i].seq = 0;
    }
    
    available_slots = WINDOW_SIZE;
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
    
    file = fopen(destinationFile, "a");
    
    if (file == NULL){
        printf("reliable_receiver: Unable to create the destination file\n");
        exit(1);
    }
    
    fclose(file);
    
    memcpy(destination, destinationFile, strlen(destinationFile));
    destination[strlen(destinationFile)] = 0;
    
	int sockfd = establish_receive_connection();
	
	pthread_t thread;
	pthread_create(&thread, NULL, (void*)write_handler,(void*)NULL);
	
	while (1) {
		receivePacket(sockfd);
	}
}

void *write_handler(void *datapv){

	while(1){
		pthread_mutex_lock(&window_lock);	
		write_to_file();	
		pthread_mutex_unlock(&window_lock);
		
		usleep(100000);
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
	
	struct packet_info *packet;
	packet = malloc(sizeof(struct packet_info));
	
	packet->hostname = malloc(256);
	memcpy(packet->hostname, sender_host, strlen(sender_host));
	packet->hostname[strlen(sender_host)] = 0;
	
	packet->datagram = malloc(sizeof(*dgram));
	*(packet->datagram) = *dgram;
	
	//pthread_t thread;
	//pthread_create(&thread, NULL, (void*)packet_handler, (void*)packet);
	
	//printf("Thread id: %u\n", thread);
	
	packet_handler((void*)packet);
	
}

void *packet_handler(void *datapv){
	
    struct packet_info *packet =  (struct packet_info *)datapv;
 
    reliable_dgram *dgram = packet->datagram;
    
    pthread_mutex_lock(&window_lock);
     
    if (available_slots > 0){ 
        
		if (dgram->seq > (window_start + WINDOW_SIZE)){
			printf("Packet with seq #%d out of bound for window\n",dgram->seq);
			return;
		}
	
        available_slots--;
        
	    dgram->payload[DATA_SIZE] = 0;
	
	    process_packet(packet);
	
	    //write_to_file();
	}
	else{
		printf("Dropping packet seq #%d\n", dgram->seq);
	}
	
	pthread_mutex_unlock(&window_lock);
}

void sendAck(char* hostName, int seq, int slots) {

    struct reliable_dgram dgram;
    
    dgram.seq = seq;
    dgram.size = 3*sizeof(char);
	dgram.window_size = slots;
	
    memcpy(dgram.payload,"ACK",dgram.size);
    
    //printf("reliable_receiver: Sending ACK for seq #%d\n", seq);
    send_dgram(hostName, send_port, &dgram);
}

/*
*   store packet int the window
*/
void process_packet(struct packet_info *packet){

    reliable_dgram *dgram = packet->datagram;
	
    //store packet in window
    int slot = map_seq_to_window(dgram->seq);
    
    if (window[slot].received == 1 && window[slot].written == 0){
		printf("reliable_receiver: Attempt to override packet not yet consumed\n");
		return;
	}

	window[slot].received = 1;
	window[slot].written = 0;
	window[slot].ack = 0;
	window[slot].seq = dgram->seq;
	
	window[slot].data = malloc(sizeof(char)*strlen(dgram->payload));
	memcpy(window[slot].data, dgram->payload, strlen(dgram->payload));
	(window[slot].data)[strlen(dgram->payload)] = 0;
	
	window[slot].hostname = malloc(sizeof(char)*strlen(packet->hostname));
	memcpy(window[slot].hostname, packet->hostname, strlen(packet->hostname));
	(window[slot].hostname)[strlen(packet->hostname)] = 0;

	//window_start++;
	//available_slots--;
}

/*
*Writes the provided data to the destination file
*/
void write_to_file(){

    //pthread_mutex_lock(&file_lock);
    
    int i = 0;
    int written_count = 0;
    
    //only write if we have packets in the correct order
	
	for(i = window_start; i < window_start + WINDOW_SIZE; i++){
    
	//while(window[map_seq_to_window(window_start)].received == 1 && 
	//	  window[map_seq_to_window(window_start)].ack == 0 &&
	//	  window[map_seq_to_window(window_start)].written == 0){
    
        int idx =  map_seq_to_window(i);
		
		if (window[idx].received == 1 && window[idx].written == 1){
			window_start++;
		}
		
		if (window[idx].received == 0)
			break;
            
		printf("reliable_receiver: writting seq#%d of size %d to file from slot %d\n", window[idx].seq, strlen(window[idx].data), idx);
		
		file = fopen(destination,"a");  
		fputs(window[idx].data, file);
		fclose(file);
		
		available_slots++;
		
		//send the ack
		sendAck(window[idx].hostname, window[idx].seq, available_slots);
		
		window[idx].received = 0;
		free(window[idx].data);
		window[idx].data = NULL;

		free(window[idx].hostname);
		window[idx].hostname = NULL;
		
		window[idx].seq = 0;
		window[idx].ack = 0;
		
		written_count++;
		
		//move the window
		window_start++;
    }
	
    //mark the index in the sliding window of what we need to write next
    next_non_written += written_count;
    
    //wrap it around
    next_non_written = map_seq_to_window(next_non_written);
 
    //pthread_mutex_unlock(&file_lock);
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

