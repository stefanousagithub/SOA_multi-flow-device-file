/**
 * Case 1: Simple, without concurrency, well documented in stdout, configurable
 * Execute the device driver in a simple way
 * There isn't concurrency on the device driver.
 * The main function of the driver are exploted:
 * - Open the file 
 * - Change the configuration, with ioctl function. 
 *   Changing the 3 values: Timeout, priority state and blocking state
 * - Write and read operation
 * All the results are printed in standard output
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

// Configurable data
#define DATA "ciao"		// buffer to write inside the file driver
#define SIZE strlen(DATA)
#define NUMWRITE 2
#define NUMREAD 3

typedef struct _input_thread {
	int id;
	char path[128];
} input_thread;


void * the_thread(void* val){

	char* device;
	int fd;
	int ret;
	int i;
	int id;

	input_thread* s = (input_thread*)val;
	device = s->path;
	id = s->id;

	sleep(1);

	// Open the file
	printf("[threadID, Device] = [%d, %s]: Opening device\n", id, device);
	fd = open(device,O_RDWR);
	if(fd == -1) {
		printf("[threadID, Device] = [%d, %s]: Error to open the device\n", id, device);
		return NULL;
	}
	printf("[threadID, Device] = [%d, %s], Device successfully opened\n", id, device);
	
	
	// Change the default configuration. Random selected
 	int config[3];
 	config[0] = 100 + rand() % 300; 
 	config[1] = rand() % 2;
 	config[2] = rand() % 2;

	ret = ioctl(fd, WR_TIMEOUT, &config[0]);
	ret = ioctl(fd, IOWR_PRIORITYSTATE, &config[1]);
	ret = ioctl(fd, IOWR_BLOCKINGSTATE, &config[2]);
	
	
	// 2 write operations
	for(i=0;i<NUMWRITE;i++){
	  ret = write(fd,DATA,SIZE);
	  if (ret == -1) printf("[threadID, Device] = [%d, %s]: write %d Error\n", id, device, i);
	  else{
	 	printf("[threadID, Device] = [%d, %s], write %d correctly executed, NumBytes copied = %d\n", id, device, i, ret);
	  } 
	}
	sleep(1);
	// 3 Read operations
	for(i=0;i<NUMREAD;i++){
		char* buff_read = (char*) malloc(SIZE);
		ret = read(fd,buff_read,SIZE);
		printf("[threadID, Device] = [%d, %s], read %d, NumBites copied = %d: values = %s\n", id, device, i, ret, buff_read);
		free(buff_read);
	};

	return NULL;

}
int main(int argc, char** argv){
     int i;
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
	pthread_create(&tid,NULL,the_thread,&var[i]);
     }

     pause();
     return 0;

}

//for(int j = 0; j < 5; j++) pthread_create(&tid,NULL,the_thread,strdup(buff));
