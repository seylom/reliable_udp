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

#include "helper.h"

#define MAXBUFLEN 1500

void reliablyReceive(unsigned short int myUDPport, char* destinationFile);
void sigchld_handler(int s);
int establish_receive_connection();
int establisb_send_connection(char* hostname);
void send_dgram(char *host, char *port, reliable_dgram *dgram);

struct sockaddr_storage their_addr;

/* Global variable storing current port in use */
char port[6];
int socket_back_to_sender = -1;
char send_port[6] ;
struct addrinfo *client_info;

int main(int argc, char** argv) {
	unsigned short int udpPort;

	if (argc != 3) {
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	udpPort = (unsigned short int) atoi(argv[1]);
	sprintf(port, "%d", udpPort);
	sprintf(send_port, "%d", udpPort + 5);

	reliablyReceive(udpPort, argv[2]);
	return 0;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*) sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*) sa)->sin6_addr);
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {

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
	
	reliable_dgram *dgram = (reliable_dgram *)buf;
	
	char* sender_host = inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
	
	//printf("reliable_receiver: got packet from %s\n", sender_host);
	//printf("reliable_receiver: received dgram with seq #%d, size %d\n", dgram->seq, dgram->size);
	//printf("reliable_receiver: packet is %d bytes long\n", numbytes);
	
	buf[numbytes] = '\0';
	
	//send ack for packet
	sendAck(sender_host, dgram->seq);
}

void sendAck(char* hostName, int seq) {

    struct reliable_dgram dgram;
    
    dgram.seq = seq;
    dgram.size = 3*sizeof(char);
    memcpy(dgram.payload,"ACK",dgram.size);
    
    printf("reliable_receiver: Sending ACK for seq #%d\n", seq);
    send_dgram(hostName, send_port, &dgram);
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

