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
#include <sys/stat.h>
#include <fcntl.h>

#define DATA_SIZE 9000

typedef struct reliable_dgram{
    int seq;    //the sequence number being sent (used by the client)
    int size;   //the size of the payload
    int window_size;    //set by the server to advertize its window size
    char payload[DATA_SIZE];    //the actual data.
    int next_seq; //set to the next expected sequence; -1 indicate no more sequence.
}reliable_dgram;
