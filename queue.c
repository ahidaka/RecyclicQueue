#include <stdio.h>
#include <stdlib.h>
#include <stddef.h> //offsefof
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include "queue.h"
#include "utils.h"

#define QDEBUG (1)

static INT _Debug = QDEBUG;

#define _D if (_Debug)  

//
#define msleep(a) usleep((a) * 1000)

#define MAINBUFSIZ (1024)
#define DATABUFSIZ (256)
#define HEADER_SIZE (5)
#define CRC8D_SIZE (1)

#define RESPONSE_TIMEOUT (90)

//
//
//
#if defined(STAILQ_HEAD)
#undef STAILQ_HEAD

#define STAILQ_HEAD(name, type)                                 \
	struct name {                                                   \
	struct type *stqh_first;        /* first element */             \
	struct type **stqh_last;        /* addr of last next element */ \
	int num_control;      \
	pthread_mutex_t lock; \
	}
#endif

#define INCREMENT(a) ((a) = (((a)+1) & 0x7FFFFFFF))

#define QueueSetLength(Buffer, Length) \
	((QUEUE_ENTRY *)(Buffer - offsetof(QUEUE_ENTRY, Data)))->Length = (Length)

//typedef struct QueueHead {...} QEntry;
STAILQ_HEAD(QueueHead, QEntry);
typedef struct QueueHead QUEUE_HEAD;

struct QEntry {
	INT Number;
	INT Length;
	BYTE Data[DATABUFSIZ];
	STAILQ_ENTRY(QEntry) Entries;
};
typedef struct QEntry QUEUE_ENTRY;

QUEUE_HEAD DataQueue;     // Received Data
QUEUE_HEAD ResponseQueue; // CO Command Responce
QUEUE_HEAD ExtraQueue;    // Event, Smack, etc currently not used
QUEUE_HEAD FreeQueue;     // Free buffer

//
//
//
BOOL stop_read;
BOOL stop_action;
BOOL stop_job;
BOOL read_ready;

typedef struct _THREAD_BRIDGE {
	pthread_t ThRead;
	pthread_t ThAction;
}
THREAD_BRIDGE;

void SetThdata(THREAD_BRIDGE Tb);
THREAD_BRIDGE *GetThdata(void);
static THREAD_BRIDGE _Tb;
void SetThdata(THREAD_BRIDGE Tb) { _Tb = Tb; }
THREAD_BRIDGE *GetThdata(void) { return &_Tb; }

#define CleanUp(i)

VOID FreeQueueInit(VOID);

void MainJob(BYTE *buffer);

static UINT QDCount = 0; // Enq Data Queue
static UINT DFCount = 0; // Deq FreeQueue
static UINT RDCount = 0; // QueueData
static UINT MJCount = 0; // MainJOb count
static UINT DDCount = 0; // Deq DataQueue
static UINT EFCount = 0; // Enq FreeQueue


static const INT QueueCount = 8;
static const INT QueueTryTimes = 10;
static const INT QueueTryWait = 2; //msec
//
//
//
INT Enqueue(QUEUE_HEAD *Queue, BYTE *Buffer)
{
	QUEUE_ENTRY *qEntry;
	const size_t offset = offsetof(QUEUE_ENTRY, Data);

	_D printf("**Enqueue:%p\n", Buffer);

	qEntry = (QUEUE_ENTRY *)(Buffer - offset);

	pthread_mutex_lock(&Queue->lock);
	qEntry->Number = INCREMENT(Queue->num_control);

	STAILQ_INSERT_TAIL(Queue, qEntry, Entries);
	pthread_mutex_unlock(&Queue->lock);
	//printf("**Enqueue:%p=%d (%p)\n", newEntry->Data, newEntry->Number, newEntry);

	return Queue->num_control;
}

BYTE *Dequeue(QUEUE_HEAD *Queue)
{
	QUEUE_ENTRY *entry;
	BYTE *buffer;
	//int num;

	_D printf("**Dequeue:\n");

	if (STAILQ_EMPTY(Queue)) {
		printf("**Dequeue Empty!\n");
		return NULL;
	}
	pthread_mutex_lock(&Queue->lock);
	entry = STAILQ_FIRST(Queue);
	buffer = entry->Data;
	//num = entry->Number;
	STAILQ_REMOVE(Queue, entry, QEntry, Entries);
	pthread_mutex_unlock(&Queue->lock);

	//printf("**Dequeue list(%d):%p\n", num, buffer);

	return buffer;
}

//
void QueueData(QUEUE_HEAD *Queue, BYTE *DataBuffer, int Length)
{
#ifdef SECURE_DEBUG
	printf("+Q"); PacketDump(DataBuffer);
#endif
	_D printf("**QueueData:%p %d\n", DataBuffer, QDCount);

	//queueBuffer = calloc(DATABUFSIZ, 1);
	//if (queueBuffer == NULL) {
	//	fprintf(stderr, "calloc error\n");
	//	return;
	//}
	//memcpy(queueBuffer, DataBuffer, Length);

	QueueSetLength(DataBuffer, Length);
	Enqueue(Queue, DataBuffer);

	QDCount++; //Enq Data Queue
}

VOID FreeQueueInit(VOID)
{
	INT i;
	struct QEntry *freeEntry;
	// We need special management space for FreeQueue entries
	//const INT FreeQueueSize = DATABUFSIZ + (sizeof(struct QEntry) - DATABUFSIZ) * 2;

	for(i = 0; i < QueueCount; i++) {
		freeEntry = (struct QEntry *) calloc(sizeof(struct QEntry), 1);
		if (freeEntry == NULL) {
			fprintf(stderr, "InitializeQueue: calloc error=%d\n", i);
			return;
		}
		Enqueue(&FreeQueue, freeEntry->Data);

		EFCount++; // Enq FreeQueue
	}
}

void *ReadThread(void *arg)
{
	BYTE   *dataBuffer;
	INT count = 0;

	_D printf("**ReadThread: %d\n", RDCount);

	////dataBuffer = malloc(DATABUFSIZ);
	do {
		dataBuffer = Dequeue(&FreeQueue);
		if (dataBuffer == NULL) {
			if (QueueTryTimes >= count) {
				fprintf(stderr, "FreeQueue empty at ReadThread\n");
				return (void*) NULL;
			}
			count++;
			msleep(QueueTryWait);
		}
	}
	while(dataBuffer == NULL);

	DFCount++; // Deq FreeQueue

	//printf("**ReadThread()\n");
	while(!stop_read) {
		read_ready = TRUE;
		//rType = GetPacket(fd, dataBuffer, (USHORT) DATABUFSIZ);
		if (stop_job) {
			printf("**ReadThread breaked by stop_job-1\n");
			break;
		}

		///////////////////////////////////////
		RDCount++; //QueueData 
		*((UINT *) &dataBuffer[0]) = RDCount;
		///////////////////////////////////////

		QueueData(&DataQueue, dataBuffer, DATABUFSIZ);
	}
	////free(dataBuffer);
	
	//printf("ReadThread end=%d stop_read=%d\n", stop_job, stop_read);
	return (void*) NULL;
}

//
void *ActionThread(void *arg)
{
	BYTE *data;
	//size_t offset;
	//QUEUE_ENTRY *qentry;
	//BYTE buffer[DATABUFSIZ];
	//BYTE *buffer;

	_D printf("**ActionThread:\n");
#if 0
	buffer = malloc(DATABUFSIZ);
	if (buffer == NULL) {
		fprintf(stderr, "malloc error at ActionThread\n");
		//return OK;
		return (void*) NULL;
	}
#endif
	while(!stop_action && !stop_job) {
		data = Dequeue(&DataQueue);
		if (data == NULL)
			msleep(1);
		else {
			DDCount++; // Deq DataQUeue
			//memcpy(buffer, data, DATABUFSIZ);
			//offset = offsetof(QUEUE_ENTRY, Data);
			//qentry = (QUEUE_ENTRY *)(data - offset);
			//free(qentry);
////////
			MainJob(data);
////////
			Enqueue(&FreeQueue, data);
			EFCount++; // Enq FreeQueue
		}
	}
	//free(buffer);
    //return OK;
	return (void*) NULL;
}

//
//
void USleep(int Usec)
{
	const int mega = (1000 * 1000);
	struct timespec t;
	t.tv_sec = 0;

	int sec = Usec / mega;

	if (sec > 0) {
		t.tv_sec = sec;
	}
	t.tv_nsec = (Usec % mega) * 1000 * 2; ////DEBUG////
	nanosleep(&t, NULL);
}

void PushPacket(BYTE *Buffer)
{
	QueueData(&DataQueue, Buffer /*pb->EncBuffer*/, 
		DATABUFSIZ /*HEADER_SIZE + length + optionLength */);
}

void MainJob(BYTE *buffer)
{
	_D printf("***MainJob***:%p %d\n", buffer, MJCount++);
}
//
//
//
int main(int ac, char **av)
{
	int rtn;
	pthread_t thRead;
	pthread_t thAction;
	THREAD_BRIDGE *thdata;

	//INT limitCount = 1000;
	INT limitCount = 100;
	INT tryCount = 0;

	////////////////////////////////
	// Threads, queues, and serial pot

	STAILQ_INIT(&DataQueue);
	//STAILQ_INIT(&ResponseQueue);
	//STAILQ_INIT(&ExtraQueue);
	STAILQ_INIT(&FreeQueue);

	FreeQueueInit();

	pthread_mutex_init(&DataQueue.lock, NULL);
	//pthread_mutex_init(&ResponseQueue.lock, NULL);
	//pthread_mutex_init(&ExtraQueue.lock, NULL);

	thdata = calloc(sizeof(THREAD_BRIDGE), 1);
	if (thdata == NULL) {
		printf("calloc error\n");
		CleanUp(0);
		exit(1);
	}

	rtn = pthread_create(&thAction, NULL, ActionThread, (void *) thdata);
	if (rtn != 0) {
		fprintf(stderr, "pthread_create() ACTION failed for %d.",  rtn);
		CleanUp(0);
		exit(EXIT_FAILURE);
	}

	rtn = pthread_create(&thRead, NULL, ReadThread, (void *) thdata);
	if (rtn != 0) {
		fprintf(stderr, "pthread_create() READ failed for %d.",  rtn);
		CleanUp(0);
		exit(EXIT_FAILURE);
	}

	thdata->ThAction = thAction;
	thdata->ThRead = thRead;
	SetThdata(*thdata);

	do {
		if (DDCount > limitCount
			|| EFCount > limitCount
			|| RDCount > limitCount
			|| DFCount > limitCount
			|| QDCount > limitCount
			) {

			stop_read = stop_action = stop_job = TRUE;
			break;
		}
		else if (tryCount > 2) {

			stop_read = stop_action = stop_job = TRUE;
			break;
		}
		//msleep(100);
		msleep(10);
		tryCount++;
	}
	while(1);

	pthread_join(thAction, NULL);
	pthread_join(thRead, NULL);

#if 0
static UINT EFCount = 0; // Enq FreeQueue
static UINT DFCount = 0; // Deq FreeQueue
static UINT QDCount = 0; // Enq Data Queue
static UINT DDCount = 0; // Deq DataQueue
static UINT RDCount = 0; // QueueData
static UINT MJCount = 0; // MainJOb count
#endif

	printf("\nEF=%d DF=%d QD=%d DD=%d RD=%d MJ=%d\n",
		EFCount,
		DFCount,
		QDCount,
		DDCount,
		RDCount,
		MJCount);

	return 0;
}
