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

void reliablyReceive(unsigned short int myUDPport, char* destinationFile);
int establish_receive_connection();
int establish_send_connection(char* hostname);
void write_to_file();
void initialize_window();
int map_seq_to_window(int seq);
void *write_handler(void *datapv);

struct sockaddr_storage their_addr;
char destination[256];

char* sender_host_name = NULL;

int done = 0 ;

int send_sock;

struct window_slot{
     int ack;
     int written;
     int received;
     int seq;
     int size;
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
        window[i].size = 0 ;
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
    file = fopen(destinationFile, "w");
    
    if (file == NULL){
        printf("reliable_receiver: Unable to create the destination file\n");
        exit(1);
    }
    
	int sockfd = establish_receive_connection();
	
	pthread_t thread;
	pthread_create(&thread, NULL, (void*)write_handler,(void*)NULL);
	
	while (!done) {
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
	unsigned char buf[MAXBUFLEN];
	char s[INET6_ADDRSTRLEN];
	socklen_t addr_len = sizeof their_addr;

	if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN - 1, 0,
			(struct sockaddr *) &their_addr, &addr_len)) == -1) {
		perror("recvfrom");
		exit(1);
	}

	// 4 bytes is end of stream notification
	if(numbytes == 4) {
		sleep(2);
		done = 1 ;
	} else {
		if (sender_host_name == NULL) {
			sender_host_name = inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
			send_sock = establish_send_connection(sender_host_name);
		}

		int seq;
		int size;
		memcpy(&seq, buf, sizeof(int));
		memcpy(&size, buf + sizeof(int), sizeof(int));
		unsigned char* payload = malloc(size);
		memcpy(payload, buf + 2*sizeof(int), size);

		if (available_slots > 0){
			if (seq > (window_start + WINDOW_SIZE)){
				printf("Packet with seq #%d out of bound for window\n",seq);
				return;
			}

			available_slots--;
			//store packet in window
			int slot = map_seq_to_window(seq);
			if (window[slot].received == 1 && window[slot].written == 0){
				printf("reliable_receiver: Attempt to override packet not yet consumed\n");
				return;
			}
			window[slot].received = 1;
			window[slot].written = 0;
			window[slot].ack = 0;
			window[slot].seq = seq;
			window[slot].size = size;
			window[slot].data = payload;
		}
	}
}

void sendAck(char* hostName, int seq, int slots) {
	unsigned char* ack = malloc(sizeof(int));
	memcpy(ack, &seq, sizeof(int));
	int sentBytes;
	if (client_info) {
		if ((sentBytes = sendto(send_sock, ack, sizeof(int), 0,
				client_info->ai_addr, client_info->ai_addrlen)) == -1) {
			perror("packet send:");
			exit(1);
		}
	}
}

/*
*Writes the provided data to the destination file
*/
void write_to_file(){
    int i = 0;
    int written_count = 0;
    
    //only write if we have packets in the correct order
	for(i = window_start; i < window_start + WINDOW_SIZE; i++){
    
        int idx =  map_seq_to_window(i);
		if (window[idx].received == 1 && window[idx].written == 1){
			window_start++;
		}
		
		if (window[idx].received == 0)
			break;
            
		printf("reliable_receiver: writting seq#%d of size %d to file from slot %d\n", window[idx].seq, strlen(window[idx].data), idx);

		fwrite(window[idx].data, 1, window[idx].size , file);
		available_slots++;
		
		//send the ack
		sendAck(sender_host_name, window[idx].seq, available_slots);
		
		window[idx].received = 0;
		free(window[idx].data);
		window[idx].data = NULL;
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

void sigchld_handler(int s) {
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

