//ECE 455 Project 2 Scheduler Deadline Driven
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include "stm32f4_discovery.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

//TASK DETAILS
//type of a task
typedef enum{PERIODIC,APERIODIC}task_type;

//dd_task stores scheduler info
typedef struct{
TaskHandle_t t_handle;
    task_type type;
    uint32_t task_id;
    uint32_t release_time;
    uint32_t absolute_deadline;
    uint32_t completion_time;
}dd_task;

typedef struct dd_task_node {
    dd_task task;
struct dd_task_node *next;
}dd_task_list;

//MESSAGES TYPES
#define M_RELEASE 1  //sent by gen to dds when a task needs to be added to list to be execture
#define M_COMPLETE 2  //from user to DDS task done
#define M_GET_ACTIVE 3  //Monitor to dds give the list of tasks that need to be scheduled
#define M_GET_COMPLETE 4  //Monitor to dds give the list of tasks that are done
#define M_MISSED_DEADLINE 5    //Monitor to dds give the list of tasks that missed deadline


////////////////////////////////
//TYPES FOR MESSAGE AND REPLIES

//message format 
typedef struct{
    uint32_t m_type //what type of message is sent above

    //separate task_id for TASK DONE bc when done we only need to tell dds what the id is 
    dd_task task //this is used for TASK_RELEASE contains all information

    //create a reply que
    QueueHandle_t temp_reply_que //task makes que, sends to dds in this message, waits for reply, dds sneds reply to that que, task will recieve adn then delete
                              //note: The que is small hopefully avoid fragmetation
}message_dds;

//reply format 
    //pass a pointer to the done, scheduled, and overdue task lists

////////////////////////////////
//Queue handles 
QueueHandle_t dds_que; //que for generator, user, and monitor to send a message to the dds
QueueHandle_t task_missed_que; //dds to task gen task missed its deadline needs to be released again by gen


////////////////////////////////
//LISTs
static dd_task_list *scheduled_task_list;
static dd_task_list *done_task_list;
static dd_task_list *task_missed_list;

//*************timeout

//functions 
void dds_f_task (void *pv);
void task_generator (void *pv);
void monitor_task (void *pv);
//user tasks
void user_task_1(void *pv);
void user_task_2(void *pv);
void user_task_3(void *pv);

//helper function
void release_task_fun (TaskHandle_t handle, task_type input_type, uint32_t input_id, uint32_t input_deadline);
void task_done_fun (uint32_t id);//send the id back when task is done 


dd_task_list  *get_scheduled_task_list (void);
dd_task_list  *get_done_task_list (void);
dd_task_list *get_task_missed_list





///////////////////////////////////////////////////////////

//protected_printf
  //printf does not protect from multiple tasks trying to print at the same time(using same resource)
  //uses a mutex (lock) to ensure one task prints at a time. will block if another task has resource
  //returns number of characters printed
  //Paramaters:
    //input string is the string to print
    //... is used to pass the %d,etc to properly print

SemaphoreHandle_t print_lock;//Handle

int protected_printf(const char *input_string,...){
  int characters_printed;
  va_list variable_type_list; //holds the ... va_list can hold any number of variables
  //va_start will point a pointer at fist element of list 
  va_start(variable_type_list,input_string);
  //xSemaphoreTake is used ask if the key to the resource is available 
  //Mutex is a lock to protect the resource (printing) from multiple tasks attempting at once 
  xSemaphoreTake(print_lock, portMAX_DELAY);
    //now has accesss
  
  //going to act like printf and return number of characters printed
  //TESTING: will pass negative if something goes wrong
  int characters_printed = vprintf(input_string, variable_type_list);//vprintf used for va list
  
  xSemaphoreGive(print_lock);//release the lock on the resource
  va_end(variable_type_list);//done using this list 
  return characters_printed;
}


