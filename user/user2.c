/**
 * Case 2: concurrency, documents a complex scenario, configurable parameters.
 * Execute the device driver with multiple concurrent access to the driver files 
 * multiple concurrent thread operations on the device
 * The main function of the driver are exploted:
 * The number of bytes written/read and correct operations executed on the file
 * are printed in standard output
 **/


#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>

// Input value for ioctl function
#define WR_TIMEOUT _IOW('a','a',int*)
#define IOWR_PRIORITYSTATE _IOW('a','b',int*)
#define IOWR_BLOCKINGSTATE _IOW('a','c',int*)

char buff[4096];
// configurable parameters	
#define DATA "ciao"		// buffer to write inside the file driver
#define SIZE strlen(DATA)
#define NUMTHREADS 3           // Num Threads working on the same minor
#define NUMWRITE 100		// Num write operations for thread
#define NUMREAD 120		// Num read operations for thread	
#define RANGETIME 5		// Range of time for execution of the threads

typedef struct _input_thread {   // Argumeny for the thread
	int id;                  // Num of the thread
	char path[128];          // Device path 
} input_thread;

void * the_thread(void* val){
	char* device;
	int fd;
	int ret;
	int i;
	int id;
	int w_numBytes;   // num bytes correctly written in file
	int w_numCorrOp;  // num write operations correctly executed (0 byte is considered correct operation)
	int r_numBytes;   // num bytes correctly read in file
	int r_numCorrOp;  // num read operations correctly executed (0 byte is considered correct operation)

	input_thread* s = (input_thread*)val;
	device = s->path;
	id = s->id;

	int t = 1 + rand() % RANGETIME;
	sleep(t);

	// Open the file
	fd = open(device,O_RDWR);
	if(fd == -1) {
		printf("open error on device %s\n",device);
		return NULL;
	}
	printf("[threadID, Device] = [%d,%s] successfully opened\n", id, device);
	
	
	// Change the default configuration. Random selected
 	int config[3];
 	config[0] = 100 + rand() % 300; 
 	config[1] = rand() % 2;
 	config[2] = rand() % 2;

	ret = ioctl(fd, WR_TIMEOUT, &config[0]);
	ret = ioctl(fd, IOWR_PRIORITYSTATE, &config[1]);
	ret = ioctl(fd, IOWR_BLOCKINGSTATE, &config[2]);
	

	// 20 write operations
	for(i=0;i<NUMWRITE;i++){
	  ret = write(fd,DATA,SIZE);
	  if (ret != -1){
	  	w_numCorrOp += 1;
	  	w_numBytes += ret;
	  } 
	}
	printf("[threadID, Device] = [%d,%s], correct write = %d, numBytes copied = %d\n", id, device, w_numCorrOp, w_numBytes);
	
	sleep(1);

	// 30 read operations
	for(i=0;i<NUMREAD;i++){
		char* buff_read = (char*) malloc(SIZE);
		ret = read(fd,buff_read,SIZE);
		free(buff_read);
		if(ret != -1){
			r_numCorrOp += 1;
	  		r_numBytes += ret;
		} 
	}
	printf("[threadID, Device] = [%d,%s], correct read = %d: NumBites copied = %d\n", id, device, r_numCorrOp, r_numBytes);
	return NULL;

}
int main(int argc, char** argv){
     int i;
     int j;
     int ret;
     int major;
     int minors;
     char *path;
     pthread_t tid;

     if(argc<4){
		printf("useg: prog pathname major minors\n");
		return -1;
     }

     path = argv[1];
     major = strtol(argv[2],NULL,10);
     minors = strtol(argv[3],NULL,10);
     printf("creating %d minors for device %s with major %d\n",minors,path,major);
     
     input_thread var[minors];

     for(i=0;i<minors;i++){
		// Initialize the files
		sprintf(buff,"sudo mknod %s%d c %d %i\n",path,i,major,i);
		system(buff);
		sprintf(buff,"sudo chmod u=rwx,g=rwx,o=rwx %s%d\n",path,i);
		system(buff);
		
		// Create the threads
		sprintf(buff,"%s%d",path,i);
		var[i].id = i;
		strcpy(var[i].path, strdup(buff));
		for(j = 0; j < NUMTHREADS; j++) {
			pthread_create(&tid,NULL,the_thread,&var[i]);
	     }
	}

     pause();
     return 0;

}


