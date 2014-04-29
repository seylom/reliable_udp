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

#define WINDOW_SIZE 40
#define PACKET_SIZE 1472
#define INT_SIZE sizeof(int)
#define HEADER_SIZE 2*INT_SIZE
#define TIMEOUT 2

void reliablyTransfer(char* hostname, unsigned short int hostUDPport,
		char* filename, unsigned long long int bytesToTransfer);
void sendPacket(char* packet);
int establisb_send_connection(char* host);
int establish_receive_connection();
int is_window_entry_timedout(int index);
void *listen_for_ack(void* data);
int window_has_room();
struct addrinfo hints, *servinfo, *p;

int send_socket;
int receive_socket;
int current_seq = 0;
int window_start = -1;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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
	return index;
}

void init(char* filename, int udpPort) {
	int i = 0;
	for (i = 0; i < WINDOW_SIZE; i++) {
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

	/* Start listening for ack */
	pthread_t thread;
	pthread_create(&thread, NULL, (void*) listen_for_ack, NULL);

	/* Loop through the file content and send packets to fill a window */
	while (1) {
		// Sending next packet if there is room in sliding window
		if (feof(fp)) {
			printf("file is sent completely\n");
			sleep(2);
			//TODO: check if all packets are acked successfully then exit.
		} else if (window_has_room()) {
			pthread_mutex_lock(&lock);
			char* packet = calloc(PACKET_SIZE, 1);
			char* data_block = malloc(PACKET_SIZE - HEADER_SIZE);
			size_t content_size = fread(data_block, 1,
					PACKET_SIZE - HEADER_SIZE, fp);

			/* Copy sequence number  to packet */
			memcpy(packet, &current_seq, INT_SIZE);

			/* Copy payload size to packet */
			memcpy(packet + INT_SIZE, &content_size, INT_SIZE);

			/* Copy payload to packet */
			memcpy(packet + HEADER_SIZE, data_block, PACKET_SIZE - HEADER_SIZE);

			/* Create a window entry */
			int index = map_seq_to_window(current_seq);
			window[index].seq = current_seq;
			window[index].data = packet;
			window[index].ack = 0;
			window[index].time_sent = (int) time(NULL);

			sendPacket(packet);
			current_seq++;
			pthread_mutex_unlock(&lock);
		} else {
			printf("Window is full\n");
			sleep(2);
		}
		// check sent packets and re-send timed out ones
		int i = 0;
		for (i = 0; i < WINDOW_SIZE; i++) {
			struct SlidingWindow entry = window[i];
			if (entry.data && is_window_entry_timedout(i)) {
				printf("packet %d is timed out.\n", entry.seq);
				entry.time_sent = (int) time(NULL);
				sendPacket(entry.data);
			}
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

void ack_packet(int seq) {
	pthread_mutex_lock(&lock);
	int index = map_seq_to_window(seq);
	struct SlidingWindow entry = window[index];
	entry.ack = 1;
	entry.seq = 0;
	free(entry.data);
	entry.data = NULL;

	// Slide window as more packets get ack
	while (window[map_seq_to_window(window_start + 1)].ack) {
		window_start++;
	}
	pthread_mutex_unlock(&lock);
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
	//printf("listener: waiting to recvfrom...\n");
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

void sendPacket(char* packet) {
	char buffer[PACKET_SIZE + 1];
	int seq;
	int size;
	memcpy(&seq, packet, sizeof(int));
	memcpy(&size, packet + sizeof(int), sizeof(int));
	memcpy(buffer, packet + HEADER_SIZE, PACKET_SIZE);
	buffer[PACKET_SIZE] = '\0';
	printf("Packet %d with size %d to send is : %s \n", seq, size, buffer);
//	int numbytes;
//	if ((numbytes = sendto(send_socket, packet, PACKET_SIZE, 0, p->ai_addr,
//			p->ai_addrlen)) == -1) {
//		perror("packet send:");
//		exit(1);
//	}
}

void *listen_for_ack(void* data) {
	printf("listening thread is started ...\n");
	receive_socket = establish_receive_connection();
	struct sockaddr_storage their_addr;
	socklen_t addr_len = sizeof their_addr;
	while (1) {
		int numbytes;
		char buf[INT_SIZE];
		if ((numbytes = recvfrom(receive_socket, buf, INT_SIZE - 1, 0,
				(struct sockaddr *) &their_addr, &addr_len)) == -1) {
			perror("ack recv");
			exit(1);
		}
		int ack_seq;
		memcpy(&ack_seq, buf, INT_SIZE);
		ack_packet(ack_seq);
	}
}