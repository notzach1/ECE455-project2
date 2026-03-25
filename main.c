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
    uint32_t m_type; //what type of message is sent above

    //separate task_id for TASK DONE bc when done we only need to tell dds what the id is 
    dd_task task; //this is used for TASK_RELEASE contains all information

    //create a reply que
    QueueHandle_t temp_reply_que; //task makes que, sends to dds in this message, waits for reply, dds sneds reply to that que, task will recieve adn then delete
                              //note: The que is small hopefully avoid fragmetation
}message_dds;

//reply format 
    //pass a pointer to the done, scheduled, and overdue task lists

////////////////////////////////
//Queue handles 
QueueHandle_t dd_scheduler_que; //que for generator, user, and monitor to send a message to the dds
QueueHandle_t task_missed_que; //dds to task gen task missed its deadline needs to be released again by gen


////////////////////////////////
//LISTs
static dd_task_list *active_task_list;
static dd_task_list *complete_task_list;
static dd_task_list *task_missed_list;

//*************timeout

//functions 
void dd_scheduler_task (void *pv);
void task_generator (void *pv);
void monitor_task (void *pv);
void t1_user (void *pv);
void t2_user (void *pv);
void t3_user (void *pv);

//user tasks
//three task handlers 


//functions for our lists 
void handle_release(message_dds *input_message);
void handle_complete(message_dds *input_message);
void handle_deadline_miss(void);
void adjust_priorities(void);

void insert_node(dd_task_list **head, dd_task task);//pass the head of the list and the task we want to put in 
dd_task_list *remove_node(dd_task_list **head, uint32_t task_id);
uint32_t list_size(dd_task_list  *head);

//functions for scheduler 
void release_dd_task (TaskHandle_t h, task_type t, uint32_t id, uint32_t deadline);
void complete_dd_task(uint32_t input_id);
dd_task_list *get_active_dd_task_list(void);
dd_task_list *get_completed_dd_task_list(void);
dd_task_list *get_overdue_dd_task_list(void);


//timeout for ques
static TickType_t que_timeout = portMAX_DELAY;

///////////////////////////////////////////////////////////
void insert_node(dd_task_list **head, dd_task task_to_add){
    dd_task_list *new_node = pvPortMalloc(sizeof(dd_task_list));//create pointer and we request memory from the heap
    new_node->task = task_to_add;
    new_node->next = NULL;

    //1) check if list is empty
    if(*head== NULL){//list is empty
        *head = new_node;//make the new node the head of the list 
        return;
    }
    //check if task to add has the earliest deadline 
    if(task_to_add.absolute_deadline <= (*head)->task.absolute_deadline){
        new_node->next = *head; //new node point at head of list 
        *head = new_node;//new node becomes start of the list with the lowest deadline
        return;
    }

    dd_task_list *list_postion = *head;//pointer to parse list
    //figure out where new node goes 
    while(list_postion->next != NULL && list_postion->next->task.absolute_deadline <= task_to_add.absolute_deadline){
        list_postion = list_postion->next;//next element of list 

    }
    new_node->next = list_postion->next; //new node tied to next of current postion
    list_postion->next= new_node;//finishes adding node 
}




dd_task_list *remove_node(dd_task_list **head, uint32_t task_id_to_remove){
    //list is empty return NULL
    if(*head==NULL){
        return NULL;
    }

    //is the node at the head
    if((*head)->task.task_id == task_id_to_remove){//compare 
        //task id is at the head of the list 
        dd_task_list *removed = *head;//node for function to return
        *head = (*head)->next;//point head of the list to the 2nd element
        removed->next = NULL;//point the removed node 
        return removed;//return the popped node 
        //dont free memory because we will use same memory for the completed list 
    }
    //to remove a node from a list we need to store the node before and the after
    dd_task_list *before = *head;
    dd_task_list *after = (*head)->next;
    while(after!= NULL){
        if(after->task.task_id == task_id_to_remove){
            before->next = after->next;
            after->next  = NULL;
            return after;
        }
        before =after;
        after =after->next;
        
    }
    //task was not found in the list
    return NULL;
}
uint32_t list_size(dd_task_list *head){
    dd_task_list *list_postion = head;
    uint32_t c=0; //count
    //parse the list 
    while(list_postion !=NULL){
        c++;//increment count
        list_postion = list_postion->next;
    }
    return c;
}
///////////////////////////////////////////////////////////
//*****setting priority
//set head of list to highest priority everything else gets low priority 
void adjust_priorities(void){
    //check if its empty
    if(active_task_list==NULL){
        que_timeout= portMAX_DELAY;
        return;
    }
    //give head of list high priorirty
    vTaskPrioritySet(active_task_list->task.t_handle, 3);
    //parse list give all others low
    dd_task_list *list_position = active_task_list->next;
    while(list_position !=NULL){
        //set lowest priority 
        vTaskPrioritySet(list_position->task.t_handle, 1);
        list_postion = list_position->next;
    }

}







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
  characters_printed = vprintf(input_string, variable_type_list);//vprintf used for va list
  
  xSemaphoreGive(print_lock);//release the lock on the resource
  va_end(variable_type_list);//done using this list 
  return characters_printed;
}


