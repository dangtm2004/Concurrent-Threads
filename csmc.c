#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <linux/futex.h>
#include <syscall.h>
#include <unistd.h>
#include <semaphore.h>


typedef struct
{
	int studentId;
	int currentHelp;
} StudentType; //struct that contain student data

typedef struct
{
	int studentsNum;
	int finished;
	StudentType students[0]; //flexible array
} Students; //contains all the students

typedef struct
{
	int tutorId;
} TutorType; //struct that contain tutor data, its made this way in purpose of future development, if there will be more data about tutor

typedef struct
{
	int tutorsNum;
	TutorType tutors[0]; //flexible array
} Tutors; //contains all the tutors

typedef struct
{
	int CoordId;
} Coordonator; //struct that contain coordinator data, its made this way in purpose of future development, if there will be more data about coordonator

typedef struct
{
	int TotalChairs;
	int BusyChairs;
	struct
	{
		int StudentId;
		int StudentAttempts;
	} *queue;
} Chairs; //struct used for chairs and the queue of the students sitting on the chair

typedef struct
{
	int queueLength;
	struct
	{
		int studentId;
		int studentAttempts;
	} *StudentsList;
} PendingQueueStudents; //struct used for the queue of the new arrived students

//Data declaration
Students *students = NULL; //pointer for students

Tutors *tutors = NULL; //pointer for tutors
int tutoredNow = 0; //counter used to 
int *whichTutor; // pointer/array (this will be allocated) to store which tutor tutored a student
int TotalTutored = 0; //global counter that count how many students were tutored
Chairs *chairs = NULL; // pointer for chairs
PendingQueueStudents pendingQueueStudents = {.StudentsList = NULL}; //new arrived students queue
int MaxHelp = -1; //number of max help that the students can get

int TotalRequests = 0; //counter for total requests

pthread_mutex_t FinishStudent; //mutex variable used to lock a memory zone to avoid multiple writes in the same time (used for students)
pthread_mutex_t ChairsQueueBusy; //mutex variable used to lock a memory zone to avoid multiple writes in the same time (used for chairs)
pthread_mutex_t PendingQueue; ////mutex variable used to lock a memory zone to avoid multiple writes in the same time (used for "whichTutor" array)

sem_t StudentNotifyCoord; //semaphore variable, used by students to notify the coordinator that he ocuppied a chair or he reached max number of help
sem_t CoordNotifyTutor; //semaphore variable, used by coordonator to notify the tutors that there are new students on chairs

static void IncrementFinishedStudents(void) //function that increment the number of the
//finished students (those students that reached max number of help that they can get)
{
	//Increment finished students
	pthread_mutex_lock(&FinishStudent);

	students->finished++;

	pthread_mutex_unlock(&FinishStudent);

	sem_post(&StudentNotifyCoord);
}

static bool DoneAllStudents(void) //boolean function that check if all the student reached the max number of help.
//If they (the students) all reached the max number of help the function will return true otherwise it will return false
{
	bool exit = false;

	pthread_mutex_lock(&FinishStudent);

	exit = (students->finished >= students->studentsNum);

	pthread_mutex_unlock(&FinishStudent);

	return exit;
}

static bool AvailableSeat(void) //boolean function that check if there is any free chair in the queue. 
//If there is at least one free chair the function will return true otherwise will return false
{
	pthread_mutex_lock(&ChairsQueueBusy);

	if((chairs->BusyChairs + pendingQueueStudents.queueLength) < chairs->TotalChairs)
	{
		return true;
	}

	pthread_mutex_unlock(&ChairsQueueBusy);

	return false;
}

	static float CalculateMilliseconds(void) //float function that will return a real number < 0.2
{
	return (float)((rand() % 200) / 100);
}

static void *StudentThreadHandler(void *Arg) //is the handler of the students threads,
//all the students threads will execute this function with its own data.
{
	StudentType *studentPtr = (StudentType *)Arg;

	while(true)
	{
		//the student thread will check if the student reached max number of help,
		//if true, the thread will notify the coordonator then exit.
		if(studentPtr->currentHelp >= MaxHelp)
		{
			IncrementFinishedStudents();
			pthread_exit(NULL);
		}

		usleep(CalculateMilliseconds()); //sleep for max 0.2ms (coding time)

		//the student check if there is any available seat, if there are none, he gets back to coding and will try again after that.
		if(!AvailableSeat())
		{
			printf("StudentThreadHandler: Student %d found no empty chair. Will try again later.\n",studentPtr->studentId);
			continue;
		}

		//if there are available seat, he will join that "new arrived queue"
		pendingQueueStudents.StudentsList[pendingQueueStudents.queueLength].studentId = studentPtr->studentId;
		pendingQueueStudents.StudentsList[pendingQueueStudents.queueLength].studentAttempts = studentPtr->currentHelp;
		pendingQueueStudents.queueLength++;

		TotalRequests++;

		printf("StudentThreadHandler: Student %d takes a seat. Empty chairs = %d\n",studentPtr->studentId, chairs->TotalChairs - chairs->BusyChairs - pendingQueueStudents.queueLength);
		pthread_mutex_unlock(&ChairsQueueBusy);

		//then notify the coordonator.
		sem_post(&StudentNotifyCoord);

		while(whichTutor[studentPtr->studentId] == -1)
		{
			//Do nothing
			//Wait the tutor to finish
		}

		printf("StudentThreadHandler: Student %d received help from Tutor %d\n", studentPtr->studentId, whichTutor[studentPtr->studentId]);

		//reset the value of the whichTutor.
		pthread_mutex_lock(&PendingQueue);
		whichTutor[studentPtr->studentId] = -1;
		pthread_mutex_unlock(&PendingQueue);
		//increment the number of the help he got.
		studentPtr->currentHelp++;
	}
}

static void *TutorThreadHandler(void *Arg) //is the handler of the tutors threads, all the tutors threads will execute this function with its own data.
{
	TutorType *tutorPtr = (TutorType *)Arg;

	int idx;

	while(true) //infinite loop
	{
		//first the tutor will check if all the student are done, if all the students are done then it will exit.
		if(DoneAllStudents())
		{
			sem_post(&CoordNotifyTutor);
			pthread_exit(NULL);
		}

		//initialize the data of next tutored student.
		int studentId = -1;
		int priority = -1;
		int queuePosition = -1; 

		//tutor waits the coordinator to notify that there are new students in the queue
		sem_wait(&CoordNotifyTutor);

		//in order to write in the "chairs" memory zone, the memory has to be locked so other threads couldnt access this memory zone.
		pthread_mutex_lock(&ChairsQueueBusy);

		//checks if there are busy chairs 
		if(chairs->BusyChairs > 0)
		{
			for(idx = chairs->BusyChairs - 1; idx >= 0 ; idx--)
			{
				//copy the data of the student that will be helped
				if(priority == -1)
				{
					studentId = chairs->queue[idx].StudentId;
					priority = chairs->queue[idx].StudentAttempts;
					queuePosition = idx;
				}

				if(chairs->queue[idx].StudentAttempts < priority)
				{
					studentId = chairs->queue[idx].StudentId;
					priority = chairs->queue[idx].StudentAttempts;
					queuePosition = idx;
				}
			}
		}

		//if there are no student, it will unlock the "chairs" memory zone and will wait for another notification from the coordonator.
		if(studentId == -1)
		{
			pthread_mutex_unlock(&ChairsQueueBusy);
			continue;
		}

		//if there is a student, he will decrement the number of the busy chairs
		chairs->BusyChairs--;
		chairs->queue[queuePosition].StudentId = chairs->queue[chairs->BusyChairs].StudentId;
		chairs->queue[queuePosition].StudentAttempts = chairs->queue[chairs->BusyChairs].StudentAttempts;

		__atomic_fetch_add(&tutoredNow, 1, __ATOMIC_SEQ_CST); //atomic increment of the "tutoredNow" students counter
		pthread_mutex_unlock(&ChairsQueueBusy);

		usleep(CalculateMilliseconds()); //sleep for a random number of miliseconds < 0.2(ms)

		//update the array that contains the data about which tutor tutored a student.
		pthread_mutex_lock(&PendingQueue);
		whichTutor[studentId] = tutorPtr->tutorId;
		pthread_mutex_unlock(&PendingQueue);

		//atomic "add" operation won't let multiple threads to write data in a specific memory zone.
		__atomic_fetch_add(&TotalTutored, 1, __ATOMIC_SEQ_CST); //built in function that increment atomic a counter
		__atomic_fetch_add(&tutoredNow, -1, __ATOMIC_SEQ_CST); //built in function that decrement atomic a counter

		printf("TutorThreadHandler: Student %d tutored by Tutor %d. Students tutored now = %d. Total sessions tutored = %d\n", studentId, tutorPtr->tutorId, tutoredNow, TotalTutored);
	}
}

static void *CoordThreadHandler(void *Arg) ////is the handler of the coordonator thread,
//it works as a dispatcher, notify the tutors that there are new students in the room (chairs queue)
{
	int idx;
	int pendingQueueLength;

	(void)Arg;
	while(true)
	{
		if(DoneAllStudents()) //coordonator check if all the students are done, if true it will notify the tutors then exit.
		{
			sem_post(&CoordNotifyTutor);
			pthread_exit(NULL);
		}

		//the coordonator thread waits to be notified by any student that he ocupied a chair, the student will be in the "new arrived queue"
		sem_wait(&StudentNotifyCoord);

		//the coordonator will lock the memory zone of the chairs and will copy the new students in the chairs queue
		pthread_mutex_lock(&ChairsQueueBusy);

		pendingQueueLength = pendingQueueStudents.queueLength;
		for(idx = 0; idx < pendingQueueLength; idx++)
		{
			pendingQueueStudents.queueLength--;
			chairs->queue[chairs->BusyChairs].StudentId = pendingQueueStudents.StudentsList[pendingQueueStudents.queueLength].studentId;
			chairs->queue[chairs->BusyChairs].StudentAttempts = pendingQueueStudents.StudentsList[pendingQueueStudents.queueLength].studentAttempts;
			printf("CoordThreadHandler: Student %d with priority %d in the queue. Waiting students now = %d. Total requests = %d\n", chairs->queue[chairs->BusyChairs].StudentId, 
				chairs->queue[chairs->BusyChairs].StudentAttempts, chairs->BusyChairs, TotalRequests);
//			printf("Move student(%d) on chairs (position: %d) - pending queue left: %d\n", chairs->queue[chairs->BusyChairs].StudentId, chairs->BusyChairs, pendingQueueStudents.queueLength - 1);
			chairs->BusyChairs++;

			//once copied, the coordonator will notify the tutors that there are new students in the queue.
			sem_post(&CoordNotifyTutor);
		}

		//after finishing the coordonator will unlock the memory zone of the chairs
		pthread_mutex_unlock(&ChairsQueueBusy);
	}
}

static void FreeData(void)
{
	//all pointers are checked and freed
	if(tutors)
	{
		free(tutors);
		tutors = NULL;
	}
	if(students)
	{
		free(students);
		students = NULL;
	}
	if(chairs)
	{
		if(chairs->queue)
		{
			free(chairs->queue);
			chairs->queue = NULL;
		}

		free(chairs);
		chairs = NULL;
	}
	if(whichTutor)
	{
		free(whichTutor);
		whichTutor = NULL;
	}
	if(pendingQueueStudents.StudentsList)
	{
		free(pendingQueueStudents.StudentsList);
		pendingQueueStudents.StudentsList = NULL;
	}
}

static int InitializeData(int studentsNum, int tutorsNum, int chairNum, int maxHelp)
{
	int status = 0;

	sem_init(&StudentNotifyCoord, 0, 0);
	sem_init(&CoordNotifyTutor, 0, 0);

	pthread_mutex_init(&ChairsQueueBusy, NULL);
	pthread_mutex_init(&PendingQueue, NULL);
	pthread_mutex_init(&FinishStudent, NULL);

	tutors = malloc(sizeof(*tutors) + tutorsNum * (sizeof(TutorType)));
	if(!tutors)
	{
		status = -1;
		goto error;
	}

	tutors->tutorsNum = tutorsNum;
	memset(tutors->tutors, 0, tutors->tutorsNum * sizeof(TutorType));

	students = malloc(sizeof(*students) + studentsNum * (sizeof(StudentType)));
	if(!students)
	{
		status = -1;
		goto error;
	}

	students->studentsNum = studentsNum;
	students->finished = 0;
	memset(students->students, 0, students->studentsNum * sizeof(StudentType));

	whichTutor = malloc(studentsNum * sizeof(int));
	if(!whichTutor)
	{
		status = -1;
		goto error;
	}

	pendingQueueStudents.queueLength = 0;
	pendingQueueStudents.StudentsList = malloc(studentsNum * (2 * sizeof(int)));

	chairs = malloc(sizeof(*chairs));
	if(!chairs)
	{
		status = -1;
		goto error;
	}

	chairs->TotalChairs = chairNum;
	chairs->BusyChairs = 0;
	chairs->queue = malloc(chairNum * (2 * sizeof(int)));
	
	MaxHelp = maxHelp;

error:
	if(status != 0)
	{
		FreeData();
	}

	return status;
}

static void Run(int studentsNum, int tutorsNum)
{
	int idx;

	pthread_t std[studentsNum];
	pthread_t ttr[tutorsNum];
	pthread_t coord;

	//create coordonator thread
	pthread_create(&coord, NULL, CoordThreadHandler, NULL);

	//create a thread for every student and initialize with negative value the "whichTutor" array.
	for(idx = 0; idx < students->studentsNum; idx++)
	{
		whichTutor[idx] = -1;
		students->students[idx].studentId = idx;
		students->students[idx].currentHelp = 0;
		pthread_create(&std[idx], NULL, StudentThreadHandler, (void *)&students->students[idx]);
	}

	//create a thread for every tutor
	for(idx = 0; idx < tutors->tutorsNum; idx++)
	{
		tutors->tutors[idx].tutorId = idx;
		pthread_create(&ttr[idx], NULL, TutorThreadHandler, (void *)&tutors->tutors[idx]);
	}

	//function waits for the thread specified by thread to terminate.  If that thread has already terminated, then pthread_join() returns immediately.
	pthread_join(coord, NULL);
	for(idx = 0; idx < students->studentsNum; idx++)
	{
		pthread_join(std[idx], NULL);
	}

	for(idx = 0; idx < tutors->tutorsNum; idx++)
	{
		pthread_join(ttr[idx], NULL);
	}
}


int main(int argc, char** argv) //main function of the app
{
	int studentsNum = -1;
	int tutorsNum = -1;
	int chairNum = -1;
	int maxHelp = -1;

//	studentsNum = 15;
//	tutorsNum = 2;
//	chairNum = 4;
//	maxHelp = 5;

	if (argc != 5)
	{
		printf("Fail. Example: <executable> 15 2 4 5\n");
		exit(-1);
	}

	//initialize the number of students, tutors, chairs, max help
	studentsNum=atoi(argv[1]);
	tutorsNum=atoi(argv[2]);
	chairNum=atoi(argv[3]);
	maxHelp=atoi(argv[4]);

	//validate the input values, if there are negative or 0 values the program will write an error then exit
	if(studentsNum <= 0 || tutorsNum <= 0 || chairNum <= 0 || maxHelp <= 0)
	{
		printf("Invalid input data\n");
		return(EXIT_FAILURE);
	}

	//function that alloc the necessary memory for all the data we have, in case of any error on this step the allocated memory will be freed, an error message will be printed and the program will exit
	if(InitializeData(studentsNum, tutorsNum, chairNum, maxHelp) != 0)
	{
		printf("Failed to initialize data\n");
		return(EXIT_FAILURE);
	}

	//after the initialization part, the program will run, this function creates all the necesary threads and start them.
	Run(studentsNum, tutorsNum);

	//this function will check and free all the allocated memory, its called just if the initialization fails or when the program shuts down.
	FreeData();

	return(EXIT_SUCCESS);
}

