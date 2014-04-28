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

#define WINDOW_SIZE 40
#define PACKET_SIZE 1472
#define HEADER_SIZE 4
#define TIMEOUT 2

void reliablyTransfer(char* hostname, unsigned short int hostUDPport,
		char* filename, unsigned long long int bytesToTransfer);
void sendPacket(int sockfd, char* message);
int establisb_send_connection(char* host);
int establish_receive_connection();
char* readFile(int size);
struct addrinfo hints, *servinfo, *p;

int send_socket;
int receive_socket;
int current_seq = 0;
int window_start = -1;

/* Pointer to the file to be sent */
FILE* fp;

/* Port number for sending and receiving */
char port[6];

/* Sliding Window data structure*/
struct SlidingWindow {
	int seq;
	time_t time_sent;
	int ack;
	char* data;
};
struct SlidingWindow window[WINDOW_SIZE];

/* Maps the actual sequence number to a index in sliding window */
int map_seq_to_window(int seq) {
	int index = seq % WINDOW_SIZE;
	return index ;
}

void init(char* filename, int udpPort) {
	int i=0;
	for(i=0; i < WINDOW_SIZE; i++) {
		window[i].ack = 0;
		window[i].seq = 0;
		window[i].time_sent = 0;
		window[i].data = NULL;
	}
	// Open file and keep the handle
	fp = fopen(filename, "r");

	// Convert port to string
	sprintf(port, "%d", udpPort);
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

	/* Loop through the file content and send packets to fill a window */
	while(1) {
		// Sending next packet if there is roon in sliding window
		if (window_has_room() ) {
			char* packet = malloc(PACKET_SIZE);
			char* data = readFile(PACKET_SIZE - HEADER_SIZE);
			char header[HEADER_SIZE];
			/* Header contains sequence number */
			sprintf(header,"%d",current_seq);

			/* Copy data to packet */
			memcpy(packet, data, PACKET_SIZE - HEADER_SIZE);
			/* Copy header to packet */
			memcpy(packet + (PACKET_SIZE - HEADER_SIZE), header, HEADER_SIZE);

			/* Create a window entry */
			int index = map_seq_to_window(current_seq);
			window[index].seq = current_seq;
			window[index].data = packet;
			window[index].ack = 0;
			window[index].time_sent = (int) time(NULL);

			// TODO: send packet

			current_seq++;
		}
		// check sent packets and re-send timed out ones
		int i = 0 ;
		for(i=0; i < WINDOW_SIZE; i++) {
			if (is_window_entry_timedout(i)) {
				// TODO: resend packet
			}
		}
	}

	sendPacket(send_socket,"Hello Server!");
}

int is_window_entry_timedout(int index) {
	struct SlidingWindow entry = window[index];
	int now = (int) time(NULL);
	if (now - entry.time_sent >= TIMEOUT) {
		return 1;
	}
	return 0 ;
}

int window_has_room() {
	if (window_start + WINDOW_SIZE > current_seq)
		return 1;
	return 0;
}

//void reset_window_entry(index) {
//	struct SlidingWindow entry = window[index];
//	free(entry.data);
//	entry.data = NULL;
//	entry.ack = 0;
//	entry.time_sent = 0;
//	entry.seq = 1;
//}

void ack_packet(int seq) {
	int index = map_seq_to_window(seq);
	struct SlidingWindow entry = window[index];
	entry.ack = 1;
	entry.seq = 0;
	free(entry.data);
	entry.data = NULL;

	// Slide window as more packets get ack
	while (window[map_seq_to_window(window_start + 1)].ack) {
		window_start ++;
	}
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
			perror("listener: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("listener: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		return 2;
	}
	freeaddrinfo(servinfo);
	printf("listener: waiting to recvfrom...\n");
	return sockfd;
}

int establisb_send_connection(char* hostname) {
	int sockfd;
	int rv;
	struct addrinfo hints, *servinfo, *p;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

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
	return sockfd;
}

void sendPacket(int sockfd, char* message) {
	int numbytes;
	if ((numbytes = sendto(sockfd, message, strlen(message), 0, p->ai_addr,
			p->ai_addrlen)) == -1) {
		perror("talker: sendto");
		exit(1);
	}

	freeaddrinfo(p);
	close(sockfd);
}

char* readFile(int size) {
	char* block = malloc(size);
	fread(block, 1, size, fp);
	return block;
}
