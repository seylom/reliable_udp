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
#include <time.h>
#include <pthread.h>

#include "helper.h"

#define WINDOW_SIZE 100
#define PACKET_SIZE 1470
#define INT_SIZE sizeof(int)
#define HEADER_SIZE 2*INT_SIZE
#define TIMEOUT 2
#define DATA_SIZE (PACKET_SIZE - HEADER_SIZE)

void reliablyTransfer(char* hostname, unsigned short int hostUDPport,
		char* filename, unsigned long long int bytesToTransfer);
void sendPacket(char* packet);
int establisb_send_connection(char* host);
int establish_receive_connection();
int is_window_entry_timedout(int index);
void *listen_for_ack(void* data);
int window_has_room();

struct addrinfo hints, *servinfo, *p;
struct addrinfo *sender_info, *receiver_info;

int send_socket;
int receive_socket;
int current_seq = 0;
int window_start = -1;
int num_bytes_sent = 0;
int last_seq_ack = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Pointer to the file to be sent */
FILE* fp;

/* Port number for sending and receiving */
char port[6];
char ack_port[6];

/* Sliding Window data structure*/
struct SlidingWindow {
	int seq;
	time_t time_sent;
	int ack;
	char* data;
	size_t size;
};
struct SlidingWindow window[WINDOW_SIZE];

/* Maps the actual sequence number to a index in sliding window */
int map_seq_to_window(int seq) {
	int index = seq % WINDOW_SIZE;
	return index;
}

void init(char* filename, int udpPort) {
	int i = 0;
	for (i = 0; i < WINDOW_SIZE; i++) {
		window[i].ack = 0;
		window[i].seq = 0;
		window[i].time_sent = 0;
		window[i].data = NULL;
		window[i].size = 0;
	}
	// Open file and keep the handle
	fp = fopen(filename, "r");

	// Convert port to string
	sprintf(port, "%d", udpPort);
	sprintf(ack_port,"%d", udpPort + 5);
}

int main(int argc, char** argv) {
	unsigned short int udpPort;
	unsigned long long int numBytes;
	if (argc != 5) {
		fprintf(stderr,
				"usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n",
				argv[0]);
		exit(1);
	}

	udpPort = (unsigned short int) atoi(argv[2]);
	numBytes = atoll(argv[4]);

	reliablyTransfer(argv[1], udpPort, argv[3], numBytes);
	return 0;
}

void reliablyTransfer(char* hostName, unsigned short int udpPort,
		char* fileName, unsigned long long int numBytes) {

	init(fileName, udpPort);

	/* Opens the connection for sending packets */
	send_socket = establisb_send_connection(hostName);

	/* Start listening for ack */
	pthread_t thread;
	pthread_create(&thread, NULL, (void*) listen_for_ack, NULL);
	//sleep(1);

    int notified_completed = 0;
    int expected_ack = 0;
    int read_bytes = 0;
	int total_ack_bytes = 0;
    
    printf("Max number of bytes to send: %d\n", numBytes);
    
	/* Loop through the file content and send packets to fill a window */
	while (1) {
		// Sending next packet if there is room in sliding window
		if (feof(fp) || read_bytes >= numBytes) {
			if (notified_completed == 0){
			    notified_completed = 1;
			    printf("reliable_sender: All file blocks were fully added to the window.\n");
			}
			
			//sleep(2);
			usleep(100000);
			//TODO: check if all packets are acked successfully then exit.
		} else{
		    if (window_has_room()) {
			    pthread_mutex_lock(&lock);
			    
			    int actual_data_size = 0;
			    
			    if(numBytes - read_bytes < DATA_SIZE){
			        actual_data_size = numBytes - read_bytes;
			    }
			    else{
			        actual_data_size = DATA_SIZE;
			    }
			        
			    char* packet = calloc(PACKET_SIZE + HEADER_SIZE, 1);
			    char* data_block = malloc(actual_data_size);
			    size_t content_size = fread(data_block, 1, actual_data_size, fp);
				data_block[content_size] = 0;

                read_bytes += content_size;
                
			    /* Copy sequence number  to packet */
			    memcpy(packet, &current_seq, INT_SIZE);

			    /* Copy payload size to packet */
			    memcpy(packet + INT_SIZE, &content_size, INT_SIZE);

			    /* Copy payload to packet */
			    memcpy(packet + HEADER_SIZE, data_block, content_size);

			    /* Create a window entry */
			    int index = map_seq_to_window(current_seq);
			    window[index].seq = current_seq;
			    window[index].data = packet;
			    window[index].ack = 0;
			    window[index].time_sent = (int) time(NULL);
			    window[index].size = actual_data_size;
			    
			    printf("reliable_sender: sending seq #%d - payload %d bytes| %d/%d bytes\n", current_seq, strlen(data_block),read_bytes, numBytes);
			    
			    sendPacket(packet);
			    current_seq++;
			    
			    pthread_mutex_unlock(&lock);
		    } else {
			    printf("Window is full\n");
			    //sleep(2);
				usleep(100000);
		    }
		}
		// check sent packets and re-send timed out ones
		
		int expected_ack = 0;
		
		int i = 0;
		for (i = 0; i < WINDOW_SIZE; i++) {
			struct SlidingWindow entry = window[i];
			if (entry.data && is_window_entry_timedout(i)) {
			    
				printf("packet %d is timed out.\n", entry.seq);
				entry.time_sent = (int) time(NULL);
				
				printf("reliable_sender: Re-sending seq #%d of payload size %d\n", entry.seq, entry.size);
				
				sendPacket(entry.data);
				
				expected_ack++;
			}
		}
		
		if (total_ack_bytes == numBytes){
		    printf("File successfully transferred!\n");
		    break;
		}
	}
}

int is_window_entry_timedout(int index) {
	struct SlidingWindow entry = window[index];
	int now = (int) time(NULL);
	if (now - entry.time_sent >= TIMEOUT) {
		return 1;
	}
	return 0;
}

int window_has_room() {
	if (window_start + WINDOW_SIZE > current_seq)
		return 1;
	return 0;
}

void ack_packet(int seq, int receiver_window) {
	pthread_mutex_lock(&lock);

	int index = map_seq_to_window(seq);
	
	if(window[index].ack == 0){
		struct SlidingWindow entry = window[index];
		window[index].ack = 1;
		window[index].seq = 0;
		
		int data_size = window[index].size;
		num_bytes_sent += data_size;
		
		//free(window[index].data);
		window[index].data = NULL;
	}
	else{
		printf("reliable_sender: Duplicate ACK received for seq #%d\n", seq);
	}

	// Slide window as more packets get ack
	int slide_value = 0;
	while (window[map_seq_to_window(window_start + 1)].ack && slide_value < WINDOW_SIZE-1 && receiver_window > 0) {
		slide_value++;
		window_start++;
		window[map_seq_to_window(window_start)].ack = 0; // reseting ack
		printf("reliable_sender: window start for seq# %d is set to %d\n", seq, window_start);
		
		receiver_window--;
		
		if (receiver_window == 0){
			printf("reliable_sender: Reached receiver advertized limit %d\n");
		}
	}
    
	pthread_mutex_unlock(&lock);
}

int establish_receive_connection() {
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int yes = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // set to AF_INET to force IPv4
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, ack_port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))
				== -1) {
			perror("reliable_sender: socket");
			continue;
		}
		
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("reliable_sender: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "reliable_sender: failed to bind socket\n");
		return 2;
	}	
	
	sender_info = malloc(sizeof *p);
	
	*sender_info = *p;

	freeaddrinfo(servinfo);
	//printf("listener: waiting to recvfrom...\n");
	return sockfd;
}

int establisb_send_connection(char* hostname) {
	int sockfd;
	int rv;
	struct addrinfo hints, *servinfo, *p;
	memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

	if ((rv = getaddrinfo(hostname, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol))
				== -1) {
			perror("talker: socket");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "talker: failed to bind socket\n");
		return 2;
	}
	
	receiver_info = malloc(sizeof *p);
	
	*receiver_info = *p;
	
	freeaddrinfo(servinfo);
	
	return sockfd;
}

void sendPacket(char* packet) {
	char buffer[PACKET_SIZE + 1];
	int seq;
	int size;
	memcpy(&seq, packet, sizeof(int));
	memcpy(&size, packet + sizeof(int), sizeof(int));
	memcpy(buffer, packet + HEADER_SIZE, PACKET_SIZE);
	buffer[PACKET_SIZE] = '\0';
	
	struct reliable_dgram dgram;
	
	dgram.seq = seq;
	dgram.size = size;
	dgram.next_seq = seq + 1;
	memcpy(dgram.payload, buffer, PACKET_SIZE);
	
	int numbytes;

    if (receiver_info){
	    if ((numbytes = sendto(send_socket, (char*)(&dgram), PACKET_SIZE + HEADER_SIZE, 0, 
	            receiver_info->ai_addr, 
	                receiver_info->ai_addrlen )) == -1) {
	                
		    perror("packet send:");
		    exit(1);
	    }
	}
}

void *listen_for_ack(void* data) {
	printf("listening thread is started ...\n");
	receive_socket = establish_receive_connection();
	struct sockaddr_storage their_addr;
	socklen_t addr_len = sizeof their_addr;
	while (1) {
		int numbytes;
		char buf[PACKET_SIZE];
		if ((numbytes = recvfrom(receive_socket, buf, PACKET_SIZE - 1, 0,
				(struct sockaddr *) &their_addr, &addr_len)) == -1) {
			perror("ack recv");
			exit(1);
		}
		
		//int ack_seq;

		reliable_dgram *dgram = (reliable_dgram *)buf;
		
		//memcpy(&ack_seq, buf, INT_SIZE);
		//ack_packet(ack_seq);
		ack_packet(dgram->seq, dgram->window_size);
	}
}
