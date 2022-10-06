#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include "list.h"

#define MAX_STRING_SIZE 4000
#define ENCRYPTION_KEY 5

void signalHandler(int sig);
void *keyboardInput();
void *printToScreen();
void *udpReciever(void *portNumber);
void *udpSender(void *arguments);

// helper functions
bool isValidLPAddress(char* lpAddress);
bool allCharAreDigits(char *string);
void encryptString(char *string);
void decryptString(char *string);


struct SenderArgStruct {
    int portNumber;
    char *lpAddress;
};

List *inputList;
List *outputList;
pthread_mutex_t inputListLock;
pthread_mutex_t outputListLock;

// flags for cancellation for the threads
// true = cancel, false means wait
pthread_cond_t cancellationFlag;
pthread_mutex_t cancellationLock;

char *statusString;
pthread_mutex_t statusLock;

int recieverSocketFD;

int main(int argc, char *argv[]) {
    if (argc >= 4) {

        // set localhost to correct port number
        if (!strcmp(argv[2], "localhost")) {
            argv[2] = "127.0.0.1";

        // check valid lp address
        } else if (!isValidLPAddress(argv[2])) {
            printf("Usage:\n\t./lets-talk <local port> <remote host> <remote port>\n");
            printf("Examples:\n\t./lets-talk 3000 192.168.0.513 3001\n");
            printf("\t./lets-talk 3000 some-computer-name 3001\n");
            exit(EXIT_FAILURE);
        }

        // initialize mutex lock for inputList
        if (pthread_mutex_init(&inputListLock, NULL) != 0) {
            printf("Mutex initialization failed...");
            exit(EXIT_FAILURE);
        }

        // initialize mutex lock for outputList
        if (pthread_mutex_init(&outputListLock, NULL) != 0) {
            printf("Mutex initialization failed...");
            exit(EXIT_FAILURE);
        }

        // initialize mutex lock for cancellation
        if (pthread_mutex_init(&cancellationLock, NULL) != 0) {
            printf("Mutex initialization failed...");
            exit(EXIT_FAILURE);
        }

        // initialize mutex lock for statusString
        if (pthread_mutex_init(&statusLock, NULL) != 0) {
            printf("Mutex initialization failed...");
            exit(EXIT_FAILURE);
        }

        // initialize condition for cancellation
        if (pthread_cond_init(&cancellationFlag, NULL) != 0) {
            printf("Cancellation flag initialization failed...");
            exit(EXIT_FAILURE);
        }

        // set status string as "Offline" by default
        statusString = (char *) malloc(8 * sizeof(char) + 1);
        strcpy(statusString, "Offline\n");

        inputList = List_create();
        outputList = List_create();

        // run thread that manages keyboard input
        pthread_t keyboardInputThreadID;
        pthread_create(&keyboardInputThreadID, NULL, keyboardInput, NULL);

        // set and run UDP reciever thread
        int localPortNum = atoi(argv[1]);
        pthread_t recieverThreadID;
        pthread_create(&recieverThreadID, NULL, udpReciever, (void *)&localPortNum);

        // run print to screen thread
        pthread_t printToScreenThreadID;
        pthread_create(&printToScreenThreadID, NULL, printToScreen, (void *)&inputList);

        // set arguments for UDP sender thread
        struct SenderArgStruct args;
        args.portNumber = atoi(argv[3]);
        args.lpAddress = argv[2];

        // start UDP sender thread
        pthread_t senderThreadID;
        pthread_create(&senderThreadID, NULL, udpSender, (void *)&args);

        // main threads waits to for signal before killing threads
        // cancellationFlag only changes when either user types "!exit"
        pthread_cond_wait(&cancellationFlag, &cancellationLock);

        // send a signal to all threads to cancel them one by one
        pthread_kill(keyboardInputThreadID, SIGTERM);
        pthread_join(keyboardInputThreadID, NULL);

        pthread_kill(recieverThreadID, SIGTERM);
        pthread_join(recieverThreadID, NULL);

        pthread_kill(printToScreenThreadID, SIGTERM);
        pthread_join(printToScreenThreadID, NULL);

        pthread_kill(senderThreadID, SIGTERM);
        pthread_join(senderThreadID, NULL);

        // destroy mutexes and flags used
        pthread_mutex_destroy(&inputListLock);
        pthread_mutex_destroy(&cancellationLock);
        pthread_cond_destroy(&cancellationFlag);

        free(statusString);

    } else if (argc < 4) {
        printf("Not enough arguments passed");
    }
}

// cancels thread when SIGTERM signal is sent to thread via pthread_kill()
void signalHandler(int sig) {
    if (sig == SIGTERM) {
        pthread_exit(NULL);
    }
}

// listens to keyboard input and adds it to output list
void *keyboardInput() {

    // set sigaction which allows thread to respond to pthread_kill() and exit safely
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    while (true) {
        pthread_mutex_lock(&inputListLock);

        char userInput[MAX_STRING_SIZE];
        fgets(userInput, MAX_STRING_SIZE, stdin);

        // add user input to inputList for sender thread to send to other user
        List_add(inputList, (void *)userInput);

        // if exit is typed, signal to main thread to start cancelling all threads
        if (strncmp(userInput, "!exit", 5) == 0) {
            pthread_cond_signal(&cancellationFlag);
        }

        // unlock mutex for inputList so sender thread is no longer waiting and it can start sending
        // acts as a signal to the sender thread to "wake up from sleep"
        pthread_mutex_unlock(&inputListLock);

        // NECESSARY TO TEMPORARILY BLOCK THIS THREAD to ensure sender thread context switches and obtains the lock
        // sender thread will never get a lock without this sleep
        sleep(0.5);
    }
}

// Takes strings from inputList and output to screen repeatedly (whenever they're added)
void *printToScreen() {

    // set sigaction which allows thread to respond to pthread_kill() and exit safely
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    printf("Welcome to Lets-Talk! Please type your messages now.\n");

    while (true) {

        // thread will be put to sleep until udpReciever thread unlocks (when outputList is appended to)
        // avoids busy waiting
        pthread_mutex_lock(&outputListLock);
        char* msgToPrint = List_remove(outputList);

        if (msgToPrint != NULL) {

            if (strncmp(msgToPrint, "!status", 7) == 0) {
                printf("%s", statusString);

            } else {
                printf("%s", msgToPrint);
            }

            fflush(stdout);
        }

        pthread_mutex_unlock(&outputListLock);
    }
}

// Creates a socket that listens to messages to specified port number via UDP. Stores recived messages into a list
void *udpReciever(void *portNumber) {

    // set sigaction which allows thread to respond to pthread_kill() and exit safely
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa,NULL);

    int portNumberAsInt =  *((int *)portNumber);

    // set IPv4, IP address and port number
    struct sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = INADDR_ANY; // all interfaces
    socketAddress.sin_port = htons(portNumberAsInt);

    // initialize socket
    if ((recieverSocketFD = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed!");
        exit(EXIT_FAILURE);
    }

    // bind socket to all interfaces
    if (bind(recieverSocketFD, (struct sockaddr*) &socketAddress, sizeof(socketAddress)) != 0) {
        perror("Socket binding failed!");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in otherSocketAddress;
    unsigned int length = sizeof(otherSocketAddress);

    while (true) {
        char buffer[MAX_STRING_SIZE];
        pthread_mutex_lock(&outputListLock);

        // MSG_WAITALL = blocked until recieved (i.e no busy waiting)
        int bytesRecieved = recvfrom(recieverSocketFD, (char *)buffer, MAX_STRING_SIZE, MSG_WAITALL,
                            ( struct sockaddr *) &otherSocketAddress, &length);


        // no error from recvfrom
        if (bytesRecieved > 0) {
            decryptString(buffer);
            buffer[bytesRecieved] = '\0';

            if (strncmp(buffer, "!status", 7) == 0) {
                strcpy(buffer, "!acknowledgement");

                // add to inputList and signal sender thread to sead response back to other user
                List_add(inputList, (void *) buffer);
                pthread_mutex_unlock(&inputListLock);

            } else if (strncmp(buffer, "!acknowledgement", 16) == 0) {
                pthread_mutex_lock(&statusLock);

                // change status to online and add "!status" to outputList for print thread to print
                strcpy(statusString, "Online\n");
                pthread_mutex_unlock(&statusLock);

                strcpy(buffer, "!status\n");
                List_add(outputList, (void *) buffer);

            } else {
                List_add(outputList, (void *) buffer);
            }

        // error signal from recvfrom. Only happens when sender thread closes socket as a signal to wake
        // up thist thread as there is no acknowledgement from !status
        } else {
            pthread_mutex_lock(&statusLock);

             // add "!status" to outputList for print thread to print (should be Offline by default)
            strcpy(buffer, "!status");
            List_add(outputList, (void *) buffer);

            pthread_mutex_unlock(&statusLock);

            close(recieverSocketFD);

            // re-initialize socket
            if ((recieverSocketFD = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("Socket creation failed!");
                exit(EXIT_FAILURE);
            }

            // re-bind socket to all interfaces
            if (bind(recieverSocketFD, (struct sockaddr*) &socketAddress, sizeof(socketAddress)) != 0) {
                perror("Socket binding failed!");
                exit(EXIT_FAILURE);
            }
        }

        pthread_mutex_unlock(&outputListLock);

        // NECESSARY TO TEMPORARILY BLOCK THIS THREAD to ensure printer thread context switches and obtains the lock
        // printer thread will never get a lock without this sleep
        sleep(0.5);

        // if exit is recieved, signal to main thread to start cancelling all threads
        if (strncmp(buffer, "!exit", 5) == 0) {
            pthread_cond_signal(&cancellationFlag);
        }
    }
}


// Creates a socket that sends messages to specified port number via UDP.
void *udpSender(void *arguments) {

    // set sigaction which allows thread to respond to pthread_kill() and exit safely
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    // cast to arg_struct and extract the arguments
    struct SenderArgStruct *args = arguments;
    int portNumber = args->portNumber;
    char *lpAddress = args->lpAddress;

    // create socket
    int sockFD;
    if ((sockFD = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed!");
        exit(EXIT_FAILURE);
    }

    // set IPv4, IP address and port number
    struct sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = inet_addr(lpAddress); // all interfaces
    socketAddress.sin_port = htons(portNumber);

    while (true) {

        // thread will be put to sleep until keyboardInput thread unlocks (when inputList is appended to)
        // avoids busy waiting
        pthread_mutex_lock(&inputListLock);

        while (List_count(inputList) > 0) {
            List_first(inputList);

            char* msgToSend = List_remove(inputList);
            encryptString(msgToSend);

            // send to server
            sendto(sockFD, (const char *)msgToSend, strlen((char *)msgToSend), MSG_CONFIRM,
                            (const struct sockaddr *)&socketAddress, sizeof(socketAddress));

            decryptString(msgToSend);
            if (strncmp(msgToSend, "!status", 7) == 0) {

                // give enough time for acknowldgement and lock is used to prevent race condition
                sleep(1);
                pthread_mutex_lock(&statusLock);

                // acknowledgement was NOT recieved
                // close socket as a signal to reciever thread to free its mutex lock and allow printer thread to go
                if (strncmp(statusString, "Offline\n", 8) == 0) {
                    pthread_mutex_unlock(&statusLock);


                // acknoledgement recieved. Simply just change the status to offline as the default
                } else if (strncmp(statusString, "Online\n", 7) == 0) {
                    strcpy(statusString, "Offline\n");
                    pthread_mutex_unlock(&statusLock);
                }
            }
        }

        pthread_mutex_unlock(&inputListLock);
    }
}


// helper functions
// ================

// checks if lp address is valid
bool isValidLPAddress(char* lpAddress) {

    char *token = strtok(lpAddress, ".\n");
    int i = 0;

    while (token != NULL) {

        // token must be number
        if (!allCharAreDigits(token)) {
            return false;

        // token must have 1 - 3 digits
        } else if (strlen(token) < 1 || strlen(token) > 3) {
            return false;
        }

        token = strtok(NULL, ".");
        i++;
    }

    // port number must have 4 parts
    return (i == 4);
}

// checks if all characters in a string are digits
bool allCharAreDigits(char *string) {
    for (int i = 0; i < strlen(string); i++) {

        if (!isdigit(string[i])) {
            return false;
        }
    }

    return true;
}


// encrypt string by inrementing each char by encryption key
void encryptString(char *string) {
    for (int i = 0; i < strlen(string); i++) {
        string[i] += ENCRYPTION_KEY;
    }
}

// decrypt string by derementing each char by encryption key
void decryptString(char *string) {
    for (int i = 0; i < strlen(string); i++) {
        string[i] -= ENCRYPTION_KEY;
    }
}