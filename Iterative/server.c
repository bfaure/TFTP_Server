/*
 * server.c

 * Course Name: 14:332:456-Network Centric Programming
 * Assignment: TFTP Server (Part 2)
 * Student Name: Brian Faure

Simple implementation of a Trivial FTP server. Prints all requests to terminal,
safisfies RRQ requests and ignores all others. Uses a multi-process approach
(fork()) to achieve concurrency and is able to effectively serve multiple RRQ 
requests simulataneously. The search_for_file function that is used to check
if the requested file exists (in the case of RRQ) takes an input 'allow_global'
which, if set to 0 (default), will prevent any requests for filepaths that 
include '/', as this would normally indicate the client is leaving the current 
working directory. 

command line and sends "Error 0: File Not Found" message back 

*/

// All standard imports from csapp.c in prior project
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

// simplifying function calls
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

// Creates a UDP-ready socket at the specified port number, returns -1 on error
int open_listening_socket(int port);

// Sends an error message to the client
void send_error_packet(int err_num,char* err_msg,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Sends a single data packet to client, returns -1 on error
int send_data_packet(int cli_sock,uint16_t block_num,int block_size,uint8_t *data,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Handles the sending of a specified file to client, returns -1 on error
int send_file(char* filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode);

// Waits for a message from client 
int get_client_resp(int cli_sock,packet_t *recv_packet,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Searches the working directory for the input filename, if allow_global is
// 1, the filename is allowed to be anywhere on the sytstem
int search_for_file(char *filename, int allow_global);

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

	printf("\nListening at port %d:\n\n",port_arg);
	
	int n,req_ct = 0;	
	// wait for incoming requests...
	while(1)
	{
		// prep a packet object to hold a request
		packet_t recv_packet;

		// wait until a packet is received
		n = recvfrom(l_sock,&recv_packet,sizeof(recv_packet),0,(SA*)&client_addr,(socklen_t *)&addrlen);

		// increment request count
		req_ct++; 

		// spawn child process to handle the request
		if (fork()==0)
		{
			// check for invalid read 
			if (n<0)
			{  
				printf("WARNING: Received invalid request.\n");  
				exit(0);
			}

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
					printf("WARNING: Could not identify request.\n");
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

	// creating struct to hold socket timeout options
	struct timeval tv;
	tv.tv_sec = 5; // 5 seconds
	tv.tv_usec = 0;

	// set the socket timeout
	int did_set = setsockopt(cli_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
	if (did_set<0){  printf("ERROR: Could not set socket options.\n");  }

	// send the error response back to client
	int did_send = sendto(cli_sock,&resp,strlen((char*)resp.error_t.error_msg)+5,0,(SA*)client_addr,*addrlen);
	if (did_send<0){  printf("ERROR: Could not send error message to client.\n");  }

	// close the client socket connection 
	close(cli_sock);
}

int send_data_packet(int cli_sock,uint16_t block_num,int block_size,uint8_t* data, struct sockaddr_in* client_addr, socklen_t* addrlen)
{
	// create new packet_t 
	packet_t d_packet;

	// set the opcode to 3 (for data)
	d_packet.opcode = htons(3);

	// set the block number 
	d_packet.data_t.block_num = htons(block_num);

	// copy the data into the packet 
	memcpy(d_packet.data_t.data,data,block_size);

	// send the data packet to client
	int did_send = sendto(cli_sock,&d_packet,block_size+4,0,(SA*)client_addr,*addrlen);
	if (did_send<0)
	{  
		printf("ERROR: Could not send error message to client.\n");  
		return -1;
	}
	return 1;
}

int get_client_resp(int cli_sock,packet_t *recv_packet,struct sockaddr_in* client_addr,socklen_t* addrlen)
{
	int n = recvfrom(cli_sock,recv_packet,sizeof(*recv_packet),0,(SA*)client_addr,addrlen);
	return n;
}

int send_file(char* filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode)
{
	// create socket connection to client to be used for transfer 
	int cli_sock = socket(AF_INET,SOCK_DGRAM,0);
	if (cli_sock<0)
	{  
		printf("ERROR: Could not open transfer socket.\n");  
		return -1;
	}

	// set the socket options 
	struct timeval tv;
	tv.tv_sec = 15;
	tv.tv_usec = 0;
	int did_set = setsockopt(cli_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
	if (did_set<0)
	{  
		printf("ERROR: Could not set socket options.\n");  
		return -1;
	}

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

		// track the number of attempted sends
		int num_attempts = 0;

		// try sending the packet
		while(1)
		{
			// send the data segment to the client 
			send_data_packet(cli_sock,block_num,n,buf,client_addr,addrlen);

			num_attempts++;
		
			// wait for the ack message 
			int did_ack = get_client_resp(cli_sock,&recv_packet,client_addr,addrlen);

			// if we got a response
			if (did_ack>0)
			{  
				// check if the received packet was an ACK
				if (ntohs(recv_packet.opcode)==4)
				{
					// check if the received block number was correct
					if (ntohs(recv_packet.ack_t.block_num)==block_num)
					{
						// go on to the next packet (or exit if transfer complete)
						break;
					}
				}
				// check if the received packet was an ERROR
				if (ntohs(recv_packet.opcode)==5)
				{
					// terminate execution upon receiving ERROR message
					close(cli_sock);
					fclose(file_ptr);
					printf("NOTICE: Transfer canceled by client.\n");
					return -1;
				}
			}
			// if exceeded the maximum number of attempts 
			if (num_attempts>=2)
			{
				// terminate execution 
				close(cli_sock);
				fclose(file_ptr);
				printf("NOTICE: Client stopped responding, transfer canceled.\n");
				return -1;
			}
		}

		// increment the block number 
		block_num++;

		// if we are at the end of the file, exit the loop
		if (n<512){  break;  }

		//sleep(2); // TESTING
	}
	// close the client socket connection
	close(cli_sock);
	// close the local file we transferred
	fclose(file_ptr);
	return block_num-1;
}

int search_for_file(char* filename, int allow_global)
{
	// if only allowed to access the immediate working directory 
	if (!allow_global)
	{
		// check if the requested filename contains any '/' characters 
		if (strchr(filename,'/')!=NULL)
		{
			// not allowed to leave current directory, return error
			return -1;
		}
	}

	// attempt to open the requested file
	int fd = open(filename,O_RDONLY); 
	if (fd<0)
	{
		// not able to locate the file
		close(fd);
		return 0;
	}
	else
	{	
		// able to locate the file
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
		// check if the requested file exists in the working directory
		int f_exists = search_for_file(filename,0);

		// if the requested file is not in the directory
		if (f_exists==0)
		{
			// send the error code back to client
			send_error_packet(1,"File Not Found",client_addr,addrlen);
			printf("\tCould not locate file \'%s\'\n",filename);
			return;
		}

		// if the requested file was not allowed to be accessed
		if (f_exists==-1)
		{
			// send the error code back to client
			send_error_packet(2,"Not Allowed",client_addr,addrlen);
			printf("\tClient denied access to \'%s\'\n",filename);
			return;	
		}

		// proceed to begin copying the file back to the client
		int num_blocks = send_file(filename,client_addr,addrlen,mode);
		if (num_blocks<0){  return;  }
		printf("\tSent \'%s\' to client in %d block(s)\n",filename,num_blocks);
	}

	// if the request is WRQ
	else
	{
		// have not covered this yet
		send_error_packet(0,"Not Yet Implemented",client_addr,addrlen);
		printf("\tCanceled WRQ request\n");
	}
}