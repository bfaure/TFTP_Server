/*
 
 * Course Name: 14:332:456-Network Centric Programming
 * Assignment: TFTP Server (Part 1)
 * Student Name: Brian Faure

Simple implementation of a Trivial FTP server. Prints requests to terminal
command line and sends "Error 0: File Not Found" message back to clients.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct sockaddr SA;
typedef socklen_t SLT;

// holds all the different likely structures for the received packets
typedef union 
{
	// just opcode in packet  
    uint16_t opcode; 

    // opcode and remaining data (RRQ/WRQ packet)
    struct 
    {
        uint16_t opcode;     
        uint8_t  req_instr[514]; // string filename, 1 null byte, string mode, 1 null byte
    } req_t;     

   	// opcode, 2 byte number, and remaining data (DATA packet)
    struct 
    {
        uint16_t opcode; 
        uint16_t block_num; // 2 bytes 
        uint8_t  data[512]; // remaining area
    } data_t;

    // opcode and 2 byte block number (ACK packet)
    struct 
    {
        uint16_t opcode;          
        uint16_t block_num; // 2 byte block num
    } ack_t;

    // opcode, 2 byte error code, string errmsg, and 1 null byte (ERROR packet)
    struct 
    {
        uint16_t opcode; 
        uint16_t error_code; // 2 byte error code 
        uint8_t  error_msg[512]; // string errmsg, 1 null byte
    } error_t;

} packet_t;


// creates a listening UDP socket at specified port number 
int open_tftp_listening_socket(int port)

// basic printing/returning of error messages to client
void handle_request(packet_t *recv_packet, struct sockaddr_in* client_addr, socklen_t* addrlen);

int main(int argc, char ** argv)
{
	// check arguments to ensure a port number was specified 
	if (argc!=2)
	{
		fprintf(stderr,"Usage: %s <port number>\n", argv[0]);
		exit(0);
	}

	// parse the listening port number
	int port_arg = atoi(argv[1]);

	// open a listening connection at specified port 
	int l_sock = open_tftp_listening_socket(port_arg);

	if (l_sock<0)
	{
		printf("ERROR: Could not open listening socket.\n");
		exit(0);
	}

	// create vars used to handling requests 
	struct sockaddr_in client_addr;
	socklen_t addrlen = sizeof(client_addr);

	int n,req_ct = 0; // 

	// wait for incoming requests...
	printf("\nListening at port %d:\n\n",port_arg);

	while(1)
	{
		// union containing likely message structure
		packet_t recv_packet;

		// get message from socket 
		n = recvfrom(l_sock,&recv_packet,sizeof(recv_packet),0,(SA*)&client_addr,(socklen_t *)&addrlen);

		// increment request count
		req_ct++; 

		// check for invalid read 
		if (n<0){  printf("ERROR: recvfrom returned <0 value\n");  }

		//printf("Got request of length %d\n",n);

		// convert opcode from network to host byte order 
		switch(ntohs(recv_packet.opcode))
		{
			// RRQ
			case 1:
				//printf("RRQ\n");
				handle_request(&recv_packet,&client_addr,(SLT*)&addrlen);
				break;

			// WRQ
			case 2:
				//printf("WRQ\n");
				handle_request(&recv_packet,&client_addr,(SLT*)&addrlen);
				break;

		}
	}
	return 1;
}

int open_tftp_listening_socket(int port)
{
	int sockfd = socket(AF_INET,SOCK_DGRAM,0); // create socket file descriptor
	if (sockfd<0){  return -1;  } 			   // check for error
	
	struct sockaddr_in serveraddr; // configure specs
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons((unsigned short)port); // port 

	int bindok = bind(sockfd,(SA*)&serveraddr,sizeof(serveraddr));
	if (bindok<0){  return -1;  }

	return sockfd;
}

void handle_request(packet_t *recv_packet, struct sockaddr_in* client_addr, socklen_t* addrlen)
{
	// string to print out packet details
	char *print_str = malloc(sizeof(char)*250);

	// prep string to hold either RRQ or WWQ
	char *req_type = malloc(sizeof(char)*10);
	if   (ntohs(recv_packet->opcode)==1){  sprintf(req_type,"RRQ");  }
	else                                {  sprintf(req_type,"WRQ");  }

	// prep string to hold the filename
	char *filename = malloc(sizeof(char)*256);

	// prep string to hold the operation mode
	char *mode = malloc(sizeof(char)*12);  

	// write into filename and mode strings 
	filename = (char*)recv_packet->req_t.req_instr;
	mode = (char*)(recv_packet->req_t.req_instr+strlen(filename)+1);

	// prep string to hold ip address
	char ip_address[128];
	inet_ntop(AF_INET, &(client_addr->sin_addr), ip_address, 128);

	// prep string to hold port number 
	char port_num[12];
	sprintf(port_num,"%d",ntohs(client_addr->sin_port));

	// prep overall string to print
	sprintf(print_str,"%s %s %s from %s:%s\n",req_type,filename,mode,ip_address,port_num);
	printf("%s",print_str); // print out request info 

	// now need to send the error code back to client...

	// creating new packet_t to hold error message
	packet_t resp;

	// set the opcode to 5 (for error)
	resp.opcode = htons(5);

	// temporary file not found error number
	resp.error_t.error_code = 0;

	// add the error message 
	strcpy(resp.error_t.error_msg,"File Not Found"); 

	// now need to open socket to connect back to client 
	int cli_sock = socket(AF_INET,SOCK_DGRAM,0);
	if (cli_sock<0){  printf("ERROR: Could not create client socket.\n");  }

	// setting the options for the socket (socket timeout)
	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	int did_set = setsockopt(cli_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
	if (did_set<0){  printf("ERROR: Could not set socket options.\n");  }

	// send the error response back to client
	int did_send = sendto(cli_sock,&resp,strlen(resp.error_t.error_msg)+5,0,(SA*)client_addr,*addrlen);
	if (did_send<0){  printf("ERROR: Could not send error message to client.\n");  }
}