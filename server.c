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
#include <string.h>

typedef struct sockaddr SA;
typedef socklen_t SLT;

// Holds the different possible combinations of data structures which could make
// up the packet contents. Used to aid in construction of sent packets and deconstruction
// of contents in received packets.
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

// Creates a UDP-ready socket at the specified port number
int open_listening_socket(int port);

// Sends an error message to the client
void send_error_packet(int err_num,char* err_msg,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Sends a single data packet to client 
void send_data_packet(int cli_sock,uint16_t block_num,int block_size,uint8_t *data,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Handles the sending of a specified file to client
int send_file(char* filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode);

// Waits for an ACK message from client 
int wait_for_ack(int cli_sock,packet_t *recv_packet,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Searches the working directory for the specified filename
int search_for_file(char *filename);

// Handles RRQ or WRQ requests
void handle_request(packet_t *recv_packet, struct sockaddr_in* client_addr, socklen_t* addrlen);

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////

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
	int l_sock = open_listening_socket(port_arg);

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

		// spawn child process to handle the request
		if (fork()==0)
		{

			// check for invalid read 
			if (n<0){  printf("ERROR: recvfrom returned <0 value\n");  }

			// convert opcode from network to host byte order 
			switch(ntohs(recv_packet.opcode))
			{
				// RRQ
				case 1:
					handle_request(&recv_packet,&client_addr,(SLT*)&addrlen);
					break;

				// WRQ
				case 2:
					handle_request(&recv_packet,&client_addr,(SLT*)&addrlen);
					break;

				default:
					printf("Could not identify request!\n");
					break;
			}
			exit(0); // close this (child) process
		}
	}

	// close the listening socket
	close(l_sock);

	return 1;
}

int open_listening_socket(int port)
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

void send_error_packet(int err_num, char* err_msg, struct sockaddr_in* client_addr, socklen_t* addrlen)
{
	// creating new packet_t to hold error message
	packet_t resp;

	// set the opcode to 5 (for error)
	resp.opcode = htons(5);

	// set the error code
	resp.error_t.error_code = htons(err_num);

	// add the error message 
	strcpy((char*)resp.error_t.error_msg,err_msg); 

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
	int did_send = sendto(cli_sock,&resp,strlen((char*)resp.error_t.error_msg)+5,0,(SA*)client_addr,*addrlen);
	if (did_send<0){  printf("ERROR: Could not send error message to client.\n");  }

	// close the client socket connection 
	close(cli_sock);
}

void send_data_packet(int cli_sock,uint16_t block_num,int block_size,uint8_t* data, struct sockaddr_in* client_addr, socklen_t* addrlen)
{
	// create new packet_t 
	packet_t d_packet;

	// set the opcode to 3 (for data)
	d_packet.opcode = htons(3);

	// set the correct block number 
	d_packet.data_t.block_num = htons(block_num);

	// copy the data into the packet 
	memcpy(d_packet.data_t.data,data,block_size);

	// send the data packet to client
	int did_send = sendto(cli_sock,&d_packet,block_size+4,0,(SA*)client_addr,*addrlen);
	if (did_send<0){  printf("ERROR: Could not send error message to client.\n");  }
}

int wait_for_ack(int cli_sock,packet_t *recv_packet,struct sockaddr_in* client_addr,socklen_t* addrlen)
{
	int n = recvfrom(cli_sock,recv_packet,sizeof(*recv_packet),0,(SA*)client_addr,addrlen);
	if (n<0){  printf("ERROR: Could not receive message from client.\n");  }
	return n;
}

int send_file(char* filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode)
{
	// create socket connection to client to be used for transfer 
	int cli_sock = socket(AF_INET,SOCK_DGRAM,0);
	if (cli_sock<0){  printf("ERROR: Could not open transfer socket.\n");  }

	// set the socket options 
	struct timeval tv;
	tv.tv_sec = 15;
	tv.tv_usec = 0;
	int did_set = setsockopt(cli_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
	if (did_set<0){  printf("ERROR: Could not set socket options.\n");  }

	// create packet to be hold ACK responses 
	packet_t recv_packet;

	// open the file differently depending on the specified encoding
	FILE *file_ptr;
	if (strcasecmp(enc_mode,"netascii")==0) {  file_ptr = fopen(filename,"r");  }
	else 									{  file_ptr = fopen(filename,"rb"); }

	// send as many data packets as we need for the full file (512 bytes at a time)
	uint16_t block_num = 1;
	while(1)
	{
		// create buffer to hold file contents 
		uint8_t buf[512];

		// read 512 bytes into the buffer 
		int n = fread(buf,1,512,file_ptr);

		// check to ensure we can read from file
		if (n<0)
		{
			printf("ERROR: Could not read from file.\n");
			return -1;
		}

		// send the data segment to the client 
		send_data_packet(cli_sock,block_num,n,buf,client_addr,addrlen);

		// wait until we get the correct ACK response
		while(1)
		{
			// wait for the ack message 
			wait_for_ack(cli_sock,&recv_packet,client_addr,addrlen);

			// get the response block number
			//int recv_block_num = ntohs(recv_packet.ack_t.block_num);

			// get the response opcode
			//int recv_opcode = ntohs(recv_packet.opcode);
			break;
		}

		// increment the block number 
		block_num++;

		// if we are at the end of the file, exit the loop
		if (n<512){  break;  }

		sleep(1); // TESTING
	}
	// close the client socket connection
	close(cli_sock);

	// close the local file we transferred
	fclose(file_ptr);
	
	return block_num-1;
}

int search_for_file(char* filename)
{
	int fd = open(filename,O_RDONLY);
	if (fd<0)
	{
		close(fd);
		return 0;
	}
	else
	{
		close(fd);
		return 1;
	}
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

	// if the request is RRQ
	if (strcmp(req_type,"RRQ")==0)
	{
		// check if the requested file exists
		int f_exists = search_for_file(filename);

		// if the requested file is not in the directory
		if (f_exists==0)
		{
			// send the error code back to client
			send_error_packet(1,"File Not Found",client_addr,addrlen);
			printf("\tCould not locate requested file!\n");
		}

		// proceed to begin copying the file back to the client
		else
		{
			int num_blocks = send_file(filename,client_addr,addrlen,mode);
			printf("\tSent requested file to client (%d blocks).\n",num_blocks);
		}
	}

	// if the request is WRQ
	else
	{
		// have not covered this yet
		send_error_packet(0,"Not Yet Implemented",client_addr,addrlen);
		printf("\tCanceled WRQ request.\n");
	}
}