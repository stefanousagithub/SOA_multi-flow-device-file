/** Specifications:
 *  Multi-flow device file
 * This specification is related to a Linux device driver implementing low and high priority flows of data. Through an open session to the device file a thread can read/write data segments. The data delivery follows a First-in-First-out policy along each of the two different data flows (low and high priority). After read operations, the read data disappear from the flow. Also, the high priority data flow must offer synchronous write operations while the low priority data flow must offer an asynchronous execution (based on delayed work) of write operations, while still keeping the interface able to synchronously notify the outcome. Read operations are all executed synchronously. The device driver should support 128 devices corresponding to the same amount of minor numbers.
 * The device driver should implement the support for the ioctl(..) service in order to manage the I/O session as follows:
 * setup of the priority level (high or low) for the operations
 * blocking vs non-blocking read and write operations
 * setup of a timeout regulating the awake of blocking operations 
 * 
 * A a few Linux module parameters and functions should be implemented in order to enable or disable the device file, in terms of a specific minor number. If it is disabled, any attempt to open a session should fail (but already open sessions will be still managed). Further additional parameters exposed via VFS should provide a picture of the current state of the device according to the following information:
 * enabled or disabled
 * number of bytes currently present in the two flows (high vs low priority)
 * number of threads currently waiting for data along the two flows (high vs low priority) 
 * 
 * Future Improvement:
 * 1) Circular Buffer
 * 2) Path control: Avoiding problems due to the presence of two files with the same minor
 * 
 **/

#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>		/* For pid types */
#include <linux/tty.h>		/* For the tty declarations */
#include <linux/version.h>	/* For LINUX_VERSION_CODE */
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/apic.h>
#include <asm/io.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include<linux/uaccess.h>             
#include <linux/kthread.h>             //kernel threads
#include <linux/delay.h>
#include <asm/atomic.h>

MODULE_LICENSE("GPL"); // (AUDIT) Da togliere?
MODULE_AUTHOR("Stefano Costanzo");

#define MODNAME "MULTI-FLOW-DRIVER"

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);

#define DEVICE_NAME "flow-device-soa"  /* Device file name in /dev/ - not mandatory  */

// Input parameters for ioctl operations
#define IOWR_PRIORITYSTATE _IOW('a','b',int*)
#define IOWR_BLOCKINGSTATE _IOW('a','c',int*)
#define IOWR_TIMEOUT _IOW('a','a',int*)

static int Major;            /* Major number assigned to broadcast device driver */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)  MAJOR(session->f_inode->i_rdev)
#define get_minor(session)  MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)  MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)  MINOR(session->f_dentry->d_inode->i_rdev)
#endif

// Struct to mananage the device file
typedef struct _object_state{
  spinlock_t operation_synchronizer;      // spin_lock to syncronize operation to the file (only one thread can access the file)
  int off_read;                           // First byte readable    
  int off_write;                          // First byte writable
  char * stream_content;                  // The I/O node is a buffer in memory
  bool priority;                          // 1 HIGH priority; 0 LOW priority
  bool blocking;                          // 1 Blocking operations;
  int timeout;                            // Timeout for blocking operation. Default value = 200 ms(DEV)
} object_state;

//
typedef struct _packed_work{
  int major;
  int minor;   
  int copiedBytes;               // To notify the thread waiting the outcome of the operation when work is completed
  struct work_struct the_work;
  char buffer[];                // Buffer to safe the bytes to be written later in the file 
} packed_work;

#define MINORS 128
object_state objects[MINORS];             // Struct for manage the device file

wait_queue_head_t read_queues[MINORS];           // Queues for blocking read operations

#define OBJECT_MAX_SIZE  (4096) //just one page writable for the device file

#define AUDIT
#define AUDITERROR

// VFS parameters
static bool enableDriver [MINORS];                // Enable state of files
static unsigned int numReaders [MINORS] = {0};    // Number readers waiting in wait_queues for the device files
static unsigned int numBytes[MINORS] = {0};       // Number bytes available in device files

module_param_array(enableDriver,bool,NULL,0660);
MODULE_PARM_DESC(enableDriver, "Enable or disable driver");
module_param_array(numReaders, uint, NULL, 0440);                             // Only readable values
MODULE_PARM_DESC(numReaders, "Number of readers waiting in the flows");
module_param_array(numBytes, uint, NULL, 0440);                               // Only readable values
MODULE_PARM_DESC(numBytes, "Number of bytes actually present in the flows");

/* the actual driver */
static int dev_open(struct inode *inode, struct file *file) {

  int minor = get_minor(file);

  if(minor >= MINORS){
    AUDITERROR
    printk("%s: [Major, Minor = %d, %d] Error in open. The minor number is not correct\n",MODNAME, get_major(file), minor);
    return -1;
  }
  
  // Deny access to disabled device file
  if (!enableDriver[minor]) {
    AUDITERROR
    printk("%s: [Major, Minor = %d, %d] Error open. The driver is disabled\n",MODNAME, get_major(file), minor);
    return -1;
  }

  AUDIT
  printk("%s: [Major, Minor = %d, %d] Device file successfully opened\n",MODNAME, get_major(file), minor);
  return 0;
}


static int dev_release(struct inode *inode, struct file *file) {
  int minor;
  minor = get_minor(file);

  AUDIT
  printk("%s: [Major, Minor = %d, %d] Device file closed\n",MODNAME, get_major(file), minor);
  //device closed by default
  return 0;

}


void delayed_work(unsigned long data){
  /** Delayed work of work_queues **/
  packed_work* the_work = container_of((void*)data,packed_work,the_work);
  int i;
  int major = the_work->major;
  int minor = the_work->minor;
  int len = the_work->copiedBytes;
  int len1 = 0;
  int len2 = 0;               // For 2nd write if off_write + len > OBJECT_MAX_SIZE (4096). Implemented for circular buffer
  char *buff1;
  char *buff2;

  object_state *the_object = objects + minor;


  spin_lock(&(the_object->operation_synchronizer));
  if((OBJECT_MAX_SIZE - the_object->off_write) >= len) {
    buff1 = (char*) &(the_work->buffer);    // unique write
    len1 = len;
  }

  else{
    // The write is divided in 2 steps due to circular form of the buffer
    len2 = len + the_object->off_write - OBJECT_MAX_SIZE;
    len1 = OBJECT_MAX_SIZE - the_object->off_write;

    buff1 = (char*) kzalloc(sizeof(char) * (len1 + 1), GFP_ATOMIC);
    buff2 = (char*) kzalloc(sizeof(char) * (len2 + 1), GFP_ATOMIC); 

    // Construct strings
    for(i = 0; i < len1; i++){
      buff1[i] = the_work->buffer[i];
    }
    for(i = 0; i < len2; i++){
      buff2[i] = the_work->buffer[i+len1];
    }
    buff1[len1] = '\0';
    buff2[len2] = '\0';
  }


  // Write from user buffer
  strcpy(&(the_object->stream_content[the_object->off_write]), buff1);
  the_object->off_write += len1;
  
  if(len2 > 0){
    the_object->off_write = 0;
    strcpy(&(the_object->stream_content[the_object->off_write]), buff2);
    the_object->off_write += len2;
    kfree(buff1);
    kfree(buff2);
  }
  spin_unlock(&(the_object->operation_synchronizer));
  
  AUDIT
  printk("%s: [Major, Minor = %d, %d] Delayed work correctly executed\n",MODNAME,major,minor);
  
  kfree(the_work);
}

ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {
  int minor;
  int ret = -1;
  int result = 0;
  object_state *the_object;
  int size_task;
  bool blocking;
  int len_2nd = 0;               // For 2nd write if offset > OBJECT_MAX_SIZE (4096). Implemented for circular buffer

  minor = get_minor(filp);
  the_object = objects + minor;
  blocking = the_object->blocking;

  AUDIT
  printk("%s: [Major, Minor = %d, %d] Somebody called a write\n",MODNAME,get_major(filp),minor);

  if(the_object->priority){
    // HIGH PRIORITY
    if(blocking){
      // BLOCKING OPERATION

      // Initilialize timer
      u64 diff;
      long long int jiffies_timer = (long long int) (get_jiffies_64() + msecs_to_jiffies(the_object->timeout));

      while(1){
        diff = jiffies_timer - (long long int)get_jiffies_64();
        if(diff < 0){
          // Timeout check
          printk("%s: [Major, Minor = %d, %d] Write timeout elapsed for thread :%d\n", MODNAME, Major, minor, current->pid);
          break;
        }
        ret = spin_trylock(&(the_object->operation_synchronizer));
        if(ret != 0) {
          if(numBytes[minor]+len >= OBJECT_MAX_SIZE){   // (DEV) Forse usare read di atomic
            // The file is full
            result = 0;
            goto exit_write;
          }

          if((OBJECT_MAX_SIZE - the_object->off_write) < len) {
              // 2nd write if offset overflow limit page (circular buffer)
              len_2nd = len + the_object->off_write - OBJECT_MAX_SIZE;
              len = OBJECT_MAX_SIZE - the_object->off_write;
          }

          // Write from user buffer
          ret = copy_from_user(&(the_object->stream_content[the_object->off_write]),buff,len);
          the_object->off_write += (len - ret); 
          result += len - ret;
          
          if(len_2nd > 0 && result == len){
            the_object->off_write=0;
            ret = copy_from_user(&(the_object->stream_content[the_object->off_write]),&buff[result],len_2nd);
            the_object->off_write += len_2nd - ret;
            result += len_2nd - ret;
          }
          goto exit_write;

        }
      }
    }
    else{
      // NOT BLOCKING OPERATION
      ret = spin_trylock(&(the_object->operation_synchronizer));
      if(ret != 0) {
        if(numBytes[minor]+len >= OBJECT_MAX_SIZE){
          // The file is full
          spin_unlock(&(the_object->operation_synchronizer));
          result = 0;
          return result;
        }

        if((OBJECT_MAX_SIZE - the_object->off_write) < len) {
            len_2nd = len + the_object->off_write - OBJECT_MAX_SIZE;
            len = OBJECT_MAX_SIZE - the_object->off_write;
        }

        // Write from user buffer
        ret = copy_from_user(&(the_object->stream_content[the_object->off_write]),buff,len);
        the_object->off_write += (len - ret);
        result += len - ret;
        
        if(len_2nd > 0 && result == len){
          // 2nd write if offset overflow limit page (circular buffer)
          the_object->off_write=0;
          ret = copy_from_user(&(the_object->stream_content[the_object->off_write]),&buff[result],len_2nd);
          the_object->off_write += len_2nd - ret;
          result += len_2nd - ret;
        }
        goto exit_write;
      }
    }
  exit_write:
    if(result > 0) atomic_add(result, (atomic_t*)&numBytes[minor]);
    spin_unlock(&(the_object->operation_synchronizer));

    wake_up(&read_queues[minor]);// Wake up the threads in read on the wait_queue
    return result;
  }

  else{
    // LOW PRIORITY
    packed_work *the_task;

    //size_task = sizeof(int)*5 + sizeof(struct work_struct) + sizeof(bool) + sizeof(char) * (len+1);   //offset of the buff inside the packed_work
    size_task = sizeof(packed_work)+ sizeof(char) * (len+1);
    the_task = (packed_work*)kzalloc(size_task,GFP_ATOMIC);    //non blocking memory allocation

    if (the_task == NULL) {
      AUDITERROR
      printk("%s: [Major, Minor = %d, %d] Tasklet buffer allocation failure\n",MODNAME, get_major(filp),minor);
      kfree(the_task);
      return -1;
    }

    // copy user buffer inside temporary buffer of delayed work
    result = copy_from_user(&(the_task->buffer),buff,len);
    the_task->buffer[len - result] = '\0';   // Put string terminator
    the_task->copiedBytes = len - result;       
    the_task->major = get_major(filp);
    the_task->minor = minor;

    if(blocking){
      // BLOCKING OPERATION

      // Initilialize timer
      long long int diff;
      long long int jiffies_timer = (long long int) (get_jiffies_64() + msecs_to_jiffies(the_object->timeout));
      while(1){
        diff = jiffies_timer - (long long int)get_jiffies_64();
        if(diff < 0){
          // Timeout check
          printk("%s: [Major, Minor = %d, %d] Write timeout elapsed for thread :%d\n", MODNAME, Major, minor, current->pid);
          goto dealloc_task;
        }
        ret = spin_trylock(&(the_object->operation_synchronizer));
        if(ret != 0) {
          if(numBytes[minor]+len >= OBJECT_MAX_SIZE){   // (DEV) Forse usare read di atomic
            // The file is full
            spin_unlock(&(the_object->operation_synchronizer));
            printk("%s: [Major, Minor = %d, %d] File is full \n",MODNAME, get_major(filp), minor);
            goto dealloc_task;
          }
          result = len - result;
          atomic_add(result, (atomic_t*)&numBytes[minor]);
          spin_unlock(&(the_object->operation_synchronizer));
          break;
        }
      }
    }

    else{
      // NOT BLOCKING OPERATION
      ret = spin_trylock(&(the_object->operation_synchronizer));
      if(ret != 0) {
        if(numBytes[minor]+len >= OBJECT_MAX_SIZE){
          // The file is full
          spin_unlock(&(the_object->operation_synchronizer));
          printk("%s: [Major, Minor = %d, %d] File is full \n",MODNAME, get_major(filp), minor);
          goto dealloc_task;
        }
        result = len - result;
        atomic_add(result, (atomic_t*)&numBytes[minor]);
        spin_unlock(&(the_object->operation_synchronizer));
      }
      else goto dealloc_task;
    }

    AUDIT
    printk("%s: [Major, Minor = %d, %d] Work buffer allocation success - the address is %p\n",MODNAME, get_major(filp), minor, the_task);

    __INIT_WORK(&(the_task->the_work),(void*)delayed_work,(unsigned long)(&(the_task->the_work)));
    schedule_work(&the_task->the_work);                                // schedule in the work queue the asyncro write process
    return result;

dealloc_task:
    kfree(the_task);       // deallocate the delayed task struct
    return result;
  }
  return -1;
}


static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {

  int minor = get_minor(filp);
  int ret;
  int result = 0;
  object_state *the_object;
  int len_2nd = 0;           // For 2nd read if offset > OBJECT_MAX_SIZE (4096). Implemented for circular buffer
  the_object = objects + minor;

  AUDIT
  printk("%s: [Major, Minor = %d, %d] Somebody called a read\n",MODNAME,get_major(filp),minor);

  atomic_inc((atomic_t*)&numReaders[minor]);

  // IF BLOCKING operations
  if(the_object->blocking){
    // Initilialize timer
    long long int jiffies_timer = (long long int)(get_jiffies_64() + msecs_to_jiffies(the_object->timeout));
    long long int diff;
    while(1){
      // Timeout check
      diff = jiffies_timer - (long long int) get_jiffies_64();
      if(diff < 0){
        AUDIT
        printk("%s: [Major, Minor = %d, %d] Timeout elapsed for thread %d\n", MODNAME, get_major(filp), minor, current->pid);
        break;
      }
      if(the_object->off_write - the_object->off_read <= 0){
        // Wait (respecting the timeout) for new bytes written to the device file
        ret = wait_event_timeout(read_queues[minor], the_object->off_write - the_object->off_read > 0 && numBytes[minor] > 0, (u64) diff);
        if(ret == 0){ 
          AUDIT
          printk("%s: [Major, Minor = %d, %d] Read timeout elapsed for thread %d\n", MODNAME, get_major(filp), minor, current->pid);
          break;
        }
      }

      ret = spin_trylock(&(the_object->operation_synchronizer));
      if(ret != 0){
        if(the_object->off_write - the_object->off_read == 0){
          spin_unlock(&(the_object->operation_synchronizer));
          continue;
        }
        if((OBJECT_MAX_SIZE - the_object->off_read) < len) {
            // 2nd write if offset overflow limit page (circular buffer)
            len_2nd = len + the_object->off_read - OBJECT_MAX_SIZE;
            len = OBJECT_MAX_SIZE - the_object->off_read;
        }
        // Read from file
        ret = copy_to_user(buff,&(the_object->stream_content[the_object->off_read]),len);
        the_object->off_read += (len - ret);
        result += len - ret;
        
        if(len_2nd > 0 && result == len){
          the_object->off_read = 0;
          ret = copy_to_user(&buff[result], &(the_object->stream_content[the_object->off_read]), len_2nd);
          the_object->off_read += len_2nd - ret;
          result += len_2nd - ret;
        }
        spin_unlock(&(the_object->operation_synchronizer));

        if(result == len){
          // Read operation is completed
          break;
        }
      }
    }
    atomic_dec((atomic_t*)&numReaders[minor]);                        // Decrement number of readers
    if(result > 0) atomic_sub(result, (atomic_t*)&numBytes[minor]);   // Decrement number readable bytes
    return result;
  }

  // IF NO BLOCKING operations
  else{
    ret = spin_trylock(&(the_object->operation_synchronizer));
    if(ret != 0){
      if(the_object->off_write - the_object->off_read == 0){
          spin_unlock(&(the_object->operation_synchronizer));
          goto exit_read;
      }
      if((OBJECT_MAX_SIZE - the_object->off_read) < len) {
          // 2nd write if offset overflow limit page (circular buffer)
          len_2nd = len + the_object->off_read - OBJECT_MAX_SIZE;
          len = OBJECT_MAX_SIZE - the_object->off_read;
        }

        // Read from file
        ret = copy_to_user(buff, &(the_object->stream_content[the_object->off_read]), len);
        the_object->off_read += (len - ret);
        result += len - ret;
        
        if(len_2nd > 0 && result == len){
          the_object->off_read = 0;
          ret = copy_to_user(&buff[result], &(the_object->stream_content[the_object->off_read]),len_2nd);
          the_object->off_read += len_2nd - ret;
          result += len_2nd - ret;
        }
        spin_unlock(&(the_object->operation_synchronizer));
      }
exit_read:
      atomic_dec((atomic_t*)&numReaders[minor]);                        // Decrement number of readers
      if(result > 0) atomic_sub(result, (atomic_t*)&numBytes[minor]);   // Decrement number readable bytes
      return result;
    }
  }

static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param) {
  int minor;
  int ret;
  int value;
  object_state *the_object;

  minor = get_minor(filp);
  the_object = objects + minor;

  ret = copy_from_user(&value, (int*)param, sizeof(int)); 
  if(ret != 0){
    AUDITERROR
    printk("%s: [Major, Minor = %d, %d] Error in ioctl, reading the input value\n", MODNAME, get_major(filp), minor);
    return -1;
  }

  // PARAM 0 (low priority) or 1 (high priority)
  switch(command){
    case IOWR_PRIORITYSTATE:
      AUDIT
      printk("%s: [Major, Minor = %d, %d] Somebody called an ioctl for priority change\n",MODNAME,get_major(filp),minor);
      if(value == 0){
        the_object->priority = false;
        return 0;
      }
      if(value == 1){
        the_object->priority = true;
        return 0;
      }
      break;

  // PARAM 0 (not blocking) or 1 (blocking)
    case IOWR_BLOCKINGSTATE:
      AUDIT
      printk("%s: [Major, Minor = %d, %d] Somebody called an ioctl for blocking change\n",MODNAME,get_major(filp),minor);

      if(value == 0){
        the_object->blocking = false;
        return 0;
      }
      if(value == 1){
        the_object->blocking = true;
        return 0;
      }
      break;

    // Change timeout
    case IOWR_TIMEOUT:  
      AUDIT
      printk("%s: [Major, Minor = %d, %d] Somebody called an ioctl for timeout change\n",MODNAME,get_major(filp),minor);
      if(value > 0){
        the_object->timeout = value;
        return 0;
      }
      else{
        AUDITERROR
        printk("%s: [Major, Minor = %d, %d] Error in ioctl, negative value\n", MODNAME, get_major(filp), minor);
        return -1;
      }  
      break;        
    }
    AUDIT
    printk("%s: [Major, Minor = %d, %d] Error in input for ioctl operation and command %u \n",MODNAME,get_major(filp),minor,command);
    return -1;
}

static struct file_operations fops = {
  .owner = THIS_MODULE,
  .write = dev_write,
  .read = dev_read,
  .open =  dev_open,
  .release = dev_release,
  .unlocked_ioctl = dev_ioctl
};

int init_module(void) {
  int i;

  // Initialize the drive internal state: Default prior. true, block. true, timeout 200ms
  for(i=0;i<MINORS;i++){
    spin_lock_init(&(objects[i].operation_synchronizer));
    objects[i].stream_content = NULL;
    objects[i].stream_content = (char*)__get_free_page(GFP_KERNEL);
    if(objects[i].stream_content == NULL) goto revert_allocationPage;
    objects[i].off_read = 0;
    objects[i].off_write = 0;
    objects[i].priority = true;           
    objects[i].blocking = true;           
    objects[i].timeout = 200;             // Default 200 ms

    enableDriver[i] = true;                     // Enable all files;
    init_waitqueue_head(&read_queues[i]);       // Initialize the wait_queues

  }

  Major = __register_chrdev(0, 0, 128, DEVICE_NAME, &fops);
  //actually allowed minors are directly controlled within this driver

  if (Major < 0) {
    AUDITERROR
    printk("%s: Registering device failed\n",MODNAME);
    return Major;
  }
  printk(KERN_INFO "%s: New device registered, it is assigned major number %d\n",MODNAME, Major);
  return 0;

revert_allocationPage:
  for(;i>=0;i--){
    free_page((unsigned long)objects[i].stream_content);
  }
  return -ENOMEM;
}

void cleanup_module(void) {

  int i;
  for(i=0;i<MINORS;i++){
    free_page((unsigned long)objects[i].stream_content);
  }
  unregister_chrdev(Major, DEVICE_NAME);
  printk(KERN_INFO "%s: New device unregistered, it was assigned major number %d\n",MODNAME, Major);

  return;
}

// asyncro blocked in write -> 
