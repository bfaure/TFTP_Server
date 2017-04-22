/*
 * server.c

 * Course Name: 14:332:456-Network Centric Programming
 * Assignment: TFTP Server (Part 2 - Concurrent)
 * Student Name: Brian Faure

Simple implementation of a Trivial FTP server. Prints all requests to terminal,
safisfies RRQ requests and ignores all others. Uses IO multiplexing in the form
of the select() function to achieve effective concurrency, allowing for serving
multiple clients at a time. By default, any RRQ requests whose filenames contain
'/' will be denied as they may indicate the client is attempting to leave the 
current working directory (this feature can be disabled by changing the 
'allow_global' input of search_for_file to 1.

When RRQ requests are receieved, I parse out all necessary information, initialize 
the first response packet, open a new transfer_t on the transfers list (just take
up a new open spot on the list in reality), and send the first 512 bytes of the file 
back to the client. All active items in the transfers list will have a chance of being
serviced on each iteration of the main event loop so long as they have information 
ready to be read from their socket.

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
#include <sys/select.h> // I/O multiplexing

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

// holds information pertinant to a pending transfer, allows for transfers to be put
// to 'sleep' while their file descriptors are not ready for reading or writing
typedef struct
{
	int socket_fd; 					// file descriptor used for transfer (socket interface)
	uint16_t block_num; 			// block index of current step of tranfer
	FILE* fp; 						// local disk file pointer
	int active; 					// 1 if this tranfer_t is being used, 0 if not
	struct sockaddr_in* client_addr;// client info
	socklen_t* addrlen; 			// client info
	int transfer_size; 				// size (bytes) of last transferred data
	char filename[200]; 			// name of file being transferred
	uint8_t cur_block[512]; 		// current block being transferred
	int block_attempts; 			// number of attempts to send current block
} transfer_t;

fd_set read_fds;           // used to maintain opened sockets for I/O multiplexing
transfer_t transfers[100]; // holds open transfer information (1 active per open socket)

// Evicts the connection at index 't' of the transfers list
void close_transfer(int t);

// Initializes all 100 spots in the transfers array to open
void init_transfers();

// Gets an index in the transfers array where the corresponding elements socket is ready to be read
int get_ready_transfer();

// Returns the highest socket fd in the transfers array 
int max_transfer_fd();

// Tries to set ownership of one of the locations in the transfers array,
// if no open spots are available returns -1
int open_transfer(int socket_fd, FILE* fp, struct sockaddr_in* client_addr, socklen_t* addrlen, char* filename);

// Reads the response from client, if correct ACK, sends next block (or terminates
// if transfer complete), if ACK is for block sent on the step before the last step
// resend the prior block. Otherwise terminate connection due to error.
void resume_transfer(int t);

// Sends the first 512 byte block of data to client
int start_transfer(char *filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode);

// Creates a UDP-ready socket at the specified port number, returns -1 on error
int open_listening_socket(int port);

// Sends an error message to the client
void send_error_packet(int err_num,char* err_msg,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Sends a single data packet to client, returns -1 on error
int send_data_packet(int cli_sock,uint16_t block_num,int block_size,uint8_t *data,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Waits for a message from client 
int get_client_resp(int cli_sock,packet_t *recv_packet,struct sockaddr_in* client_addr,socklen_t* addrlen);

// Searches the working directory for the input filename, if allow_global is
// 1, the filename is allowed to be anywhere on the sytstem
int search_for_file(char *filename, int allow_global);

// Handles RRQ or WRQ requests, for RRQ, starts transfer by sending the first block to client
// then returns the socket fd such that main can add this to the list of ******
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

	// initialize array used to hold transfer information
	init_transfers();

	// create vars used to handling requests 
	struct sockaddr_in client_addr;
	socklen_t addrlen = sizeof(client_addr);

	FD_ZERO(&read_fds); // initialize read fds

	printf("\nListening at port %d:\n\n",port_arg);
	int nfds,ready = 0;
	// wait for incoming requests...
	while(1)
	{
		// FD_SET the listening socket
		FD_SET(l_sock,&read_fds);
		// FD_SET all active socket fds
		for (int i=0; i<100; i++){ if (transfers[i].active){ FD_SET(transfers[i].socket_fd,&read_fds);}}
		
		// get the maximum file descriptor
		nfds = max_transfer_fd();
		if (l_sock>nfds){  nfds=l_sock+1;  }
		else 			{  nfds++;         }

		// filter out all non-ready sockets
		ready = select(nfds,&read_fds,NULL,NULL,NULL);

		// error in select function
		if (ready==-1)
		{
			printf("ERROR: Could not select a file descriptor.\n");
			return -1;
		}

		// no fds ready for read/write
		if (ready==0){  continue;  }

		// check if the listening socket is ready
		if (FD_ISSET(l_sock,&read_fds))
		{
			// decrement number of sockets waiting
			ready--;

			// prep a packet object to hold incoming request
			packet_t recv_packet;

			// read the data from the client
			int n = recvfrom(l_sock,&recv_packet,sizeof(recv_packet),0,
				(SA*)&client_addr,(socklen_t*)&addrlen);

			// check for errors on read
			if (n<0)
			{
				printf("WARNING: Received invalid request.\n");
				goto SERVICE_OPEN_CLIENTS; // disregard this request
			}

			// if a RRQ or WRQ request 
			if (ntohs(recv_packet.opcode)== 1 || ntohs(recv_packet.opcode)==2)
				handle_request(&recv_packet,&client_addr,(SLT*)&addrlen);
			else  
				printf("WARNING: Could not identify request.\n");
		}

		SERVICE_OPEN_CLIENTS:
		// if there are still some sockets waiting to be serviced,
		for(int i=0; i<ready; i++)
		{
			// get a transfer with a ready socket
			int selected_transfer_idx = get_ready_transfer();
			if (selected_transfer_idx==-1){  break;  }
			resume_transfer(selected_transfer_idx);
		}
	}
	// close the listening socket
	close(l_sock);
	return 1;
}

void close_transfer(int t)
{
	if (t!=-1)
	{
		FD_CLR(transfers[t].socket_fd,&read_fds);
		transfers[t].active = 0;
	}
}

void init_transfers()
{
	for (int i=0; i<100; i++)
	{
		close_transfer(i);
	}
}

int get_ready_transfer()
{
	for (int i=0; i<100; i++)
	{
		if (transfers[i].active && FD_ISSET(transfers[i].socket_fd,&read_fds))
		{
			FD_CLR(transfers[i].socket_fd,&read_fds);
			return i;
		}
	}
	return -1;
}

int max_transfer_fd()
{
	int max_fd = -1;
	for (int i=0; i<100; i++)
	{
		if (transfers[i].active && (transfers[i].socket_fd>max_fd))
		{
			max_fd = transfers[i].socket_fd;
		}
	}
	return max_fd;
}

int open_transfer(int socket_fd, FILE* fp, struct sockaddr_in* client_addr, socklen_t* addrlen, char* filename)
{
	// iterate over all possible slots in which to place the new client
	for (int i=0; i<100; i++)
	{
		// if this slot is open
		if (transfers[i].active==0)
		{
			transfers[i].socket_fd = socket_fd;
			transfers[i].block_num = 0;
			transfers[i].fp = fp;
			transfers[i].active = 1;
			transfers[i].client_addr = client_addr;
			transfers[i].addrlen = addrlen;
			transfers[i].transfer_size = -1;
			transfers[i].block_attempts = 0;
			strcpy(transfers[i].filename,filename);
			return i;
		}
	}
	printf("WARNING: Number of transfer bays maxed out.\n");
	return -1;
}

void resume_transfer(int t)
{
	//sleep(1); // TESTING

	// create packet to hold ACK responses
	packet_t recv_packet;

	int last_block_num = transfers[t].block_num;
	int ack_legit = 0;

	int resp_size = get_client_resp(transfers[t].socket_fd,&recv_packet,
		transfers[t].client_addr,transfers[t].addrlen);

	// if we got a response
	if (resp_size>0)
	{  
		// check if the received packet was an ACK
		if (ntohs(recv_packet.opcode)==4)
		{
			// check if the received block number was correct
			if (ntohs(recv_packet.ack_t.block_num)==last_block_num)
			{
				// go on to the next packet (or exit if transfer complete)
				ack_legit = 1;

				// check if this was the last transfer
				if (transfers[t].transfer_size<512)
				{
					// prep string to hold ip address
					char ip_address[128];
					inet_ntop(AF_INET, &(transfers[t].client_addr->sin_addr), ip_address, 128);

					// prep string to hold port number 
					char port_num[12];
					sprintf(port_num,"%d",ntohs(transfers[t].client_addr->sin_port));

					// print transfer confirmation to stdout
					printf("Sent \'%s\' to %s:%s in %d block(s)\n",transfers[t].filename,
						ip_address,port_num,last_block_num);

					goto RESUME_CLEANUP;
				}
			}
			else
			{
				// if we need to re-send the last block
				if (ntohs(recv_packet.ack_t.block_num)==last_block_num-1)
				{	
					// if we have already tried sending this block 3 times, cut off
					if (transfers[t].block_attempts>3)
						goto RESUME_CLEANUP;

					// try sending block again
					send_data_packet(transfers[t].socket_fd,last_block_num,
						transfers[t].transfer_size,transfers[t].cur_block,
						transfers[t].client_addr,transfers[t].addrlen);
					transfers[t].block_attempts++;
					return;
				}
				// if the block number wasn't the last or the one prior to the last
				else
				{
					printf("ERROR: Client ACK block number off >2.\n");
					goto RESUME_CLEANUP;
				}
			}
		}
		// check if the received packet was an ERROR
		if (ntohs(recv_packet.opcode)==5)
		{
			// terminate execution upon receiving ERROR message
			printf("NOTICE: Transfer canceled by client.\n");
			goto RESUME_CLEANUP;
		}
	}
	else
	{
		printf("ERROR: Invalid message from client.\n");
		goto RESUME_CLEANUP;
	}

	// if the client acknowledged the last block sent
	if (ack_legit)
	{
		int next_block_num = last_block_num+1;
		transfers[t].block_num = next_block_num;

		uint8_t buf[512];

		int n = fread(buf,1,512,transfers[t].fp);

		if (n<0)
		{
			printf("ERROR: Could not read from file.\n");
			goto RESUME_CLEANUP;
		}

		// send this block
		send_data_packet(transfers[t].socket_fd,next_block_num,n,buf,
			transfers[t].client_addr,transfers[t].addrlen);

		// set the true size of the last block transferred
		transfers[t].transfer_size = n;

		// this is the first time trying to send this block
		transfers[t].block_attempts = 1;

		// copy the current block into the transfer struct in case we need to re-send
		memcpy(transfers[t].cur_block,buf,n);
		return;
	}

	RESUME_CLEANUP:
	close(transfers[t].socket_fd); // close the open socket
	fclose(transfers[t].fp); // close the FILE pointer
	close_transfer(t); // de-activate the transfer slot
}

int start_transfer(char *filename, struct sockaddr_in* client_addr, socklen_t* addrlen, char* enc_mode)
{
	int t,cli_sock = -1; // initialize variables

	// create socket connection to client to be used for transfer 
	cli_sock = socket(AF_INET,SOCK_DGRAM,0);
	if (cli_sock<0)
	{  
		printf("ERROR: Could not open transfer socket.\n");  
		goto CLEANUP;
	}

	// set the socket options 
	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	int did_set = setsockopt(cli_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
	if (did_set<0)
	{  
		printf("ERROR: Could not set socket options.\n");  
		goto CLEANUP;
	}

	// open the file differently depending on the specified encoding
	FILE *file_ptr;
	if (strcasecmp(enc_mode,"netascii")==0) {  file_ptr = fopen(filename,"r");  }
	else 									{  file_ptr = fopen(filename,"rb"); }

	// try to occupy a transfer bay (index) in the list of 100 transfers
	t = open_transfer(cli_sock,file_ptr,client_addr,addrlen,filename);
	if (t==-1)
	{  
		printf("ERROR: Could not open a new transfer.\n");  
		goto CLEANUP;
	}

	uint16_t block_num = 1;
	transfers[t].block_num = block_num;

	uint8_t buf[512];
	int n = fread(buf,1,512,transfers[t].fp);
	if (n<0)
	{
		printf("ERROR: Could not read from file.\n");
		goto CLEANUP;
	}

	// send this block
	send_data_packet(transfers[t].socket_fd,block_num,n,buf,
		transfers[t].client_addr,transfers[t].addrlen);

	// let ourselves know that this was the final block of the transfer
	transfers[t].transfer_size = n;

	// copy the current block into the transfer struct (in case resend req.)
	memcpy(transfers[t].cur_block,buf,n);

	// first attempt to send this block
	transfers[t].block_attempts = 1; 
	return 1;

	CLEANUP:
	close(cli_sock);
	close_transfer(t);
	return -1;
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
	return recvfrom(cli_sock,recv_packet,sizeof(*recv_packet),
		0,(SA*)client_addr,addrlen);
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
	sprintf(print_str,"%s  \'%s\' %s from %s:%s\n",req_type,filename,mode,ip_address,port_num);
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
			printf("Could not locate file \'%s\'\n",filename);
			return;
		}

		// if the requested file was not allowed to be accessed
		if (f_exists==-1)
		{
			// send the error code back to client
			send_error_packet(2,"Not Allowed",client_addr,addrlen);
			printf("Client denied access to \'%s\'\n",filename);
			return;	
		}

		// proceed to begin copying the file back to the client
		start_transfer(filename,client_addr,addrlen,mode);
	}

	// if the request is WRQ
	else
	{
		// have not covered this yet
		send_error_packet(0,"Not Yet Implemented",client_addr,addrlen);
		printf("Canceled WRQ request\n");
	}
}