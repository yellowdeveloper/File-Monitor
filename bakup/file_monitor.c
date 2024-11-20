#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/inotify.h>
#include <libnotify/notify.h>
#include <pthread.h>
#include <libconfig.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

// define error code
#define EXT_SUCCESS 0
#define EXT_ERR_TOO_FEW_ARGS 1
#define EXT_ERR_INIT_INOTIFY 2
#define EXT_ERR_ADD_WATCH 3
#define EXT_ERR_BASE_PATH_NULL 4
#define EXT_ERR_READ_INOTIFY 5
#define EXT_ERR_LIBNOTIFY 6

// global variables 
int IeventStatus = -1; // inotify watch status
int IeventQueue = -1; // inotify waiting queue
char* ProgramTitle = "file_monitor"; // title of program
FILE* logFile = NULL; // log file pointer
const char* filteredExtension = NULL; // file extension to filter
int emailEnabled = 0; // email notificaton activate status
const char* emailRecipient = NULL; // email receiver
const char* logFilePath = NULL; // log file path
time_t lastEventTime = 0; // last event occur time

// recurrent function and log file size check
void rotate_log_file(const char* logPath) {
    if (logFile) {
        fclose(logFile);
    }

    // open new log file
    logFile = fopen(logPath, "a");
    if (!logFile) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    printf("Log file created/opened at: %s\n", logPath); // 디버깅 메시지
}

// signal handle function: clean up after program ends
void signal_handler(int signal) {
    printf("Signal received: cleaning up\n");

    // remove inotify watch
    if (inotify_rm_watch(IeventQueue, IeventStatus) == -1) {
        fprintf(stderr, "Error removing from watch queue!\n");
    }
    close(IeventQueue); // ends inotify waiting queue

    // close log file
    if (logFile) {
        fclose(logFile);
    }

    exit(EXIT_SUCCESS);
}

// event writing functiion (log)
void log_event(const char* eventMessage) {
    if (logFile) {
        fprintf(logFile, "Event: %s\n", eventMessage); // write event message
        fflush(logFile); // write to file buffer
    }
}

// error writing function (log)
void log_error(const char* message) {
    if (logFile) {
        fprintf(logFile, "Error: %s\n", message); // write error message
        fflush(logFile); // close log file
    }
}

// event handle fucntion
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) { // handle if file has a name
        char notificationMessage[512];
        char eventTime[64];
        time_t currentTime = time(NULL);

        // format occur time
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));
        snprintf(notificationMessage, sizeof(notificationMessage), "[%s] File %s: ", eventTime, watchEvent->name); // create message include time

        // add message by event type
        if (watchEvent->mask & IN_CREATE) {
            strcat(notificationMessage, "created");
        }
        else if (watchEvent->mask & IN_DELETE) {
            strcat(notificationMessage, "deleted");
        }
        else if (watchEvent->mask & IN_ACCESS) {
            strcat(notificationMessage, "accessed");
        }
        else if (watchEvent->mask & IN_CLOSE_WRITE) {
            strcat(notificationMessage, "written and closed");
        }
        else if (watchEvent->mask & IN_MODIFY) {
            strcat(notificationMessage, "modified");
        }
        else if (watchEvent->mask & IN_MOVE_SELF) {
            strcat(notificationMessage, "moved");
        }
        else {
            strcat(notificationMessage, "unknown event"); // handle other events
        }

        // compare time to make events to be handled by 1sec
        if (difftime(currentTime, lastEventTime) >= 1) {  // handle event at least 1sec gap
            lastEventTime = currentTime;

            // write event log
            log_event(notificationMessage);

            // print to console
            printf("%s\n", notificationMessage);
        }
    }
}

int main(int argc, char** argv) {
    char* basePath = NULL;
    char buffer[4096];
    int readLength;
    const struct inotify_event* watchEvent;
    int eventDetected = 0;

    const uint32_t watchMask = IN_CREATE | IN_DELETE | IN_ACCESS | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF; // list of events

    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    // log file initialize and read config file
    logFilePath = "file_monitor.log"; // set log file path (default)

    rotate_log_file(logFilePath); // log file initialize

    basePath = (char*)malloc(sizeof(char) * (strlen(argv[1]) + 1));
    strcpy(basePath, argv[1]); // set directory path to watch

    IeventQueue = inotify_init(); // inotify initialize
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    IeventStatus = inotify_add_watch(IeventQueue, argv[1], watchMask); // add directory watch
    if (IeventStatus == -1) {
        fprintf(stderr, "Error adding file to watchlist\n");
        exit(EXT_ERR_ADD_WATCH);
    }

    while (1) {
        if (!eventDetected) {
            printf("waiting for event...\n");
            eventDetected = 1;
        }

        readLength = read(IeventQueue, buffer, sizeof(buffer)); // read event from inotify
        if (readLength == -1) {
            fprintf(stderr, "Error reading from inotify instance\n");
            exit(EXT_ERR_READ_INOTIFY);
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength; ) {
            watchEvent = (const struct inotify_event*)buffPointer;
            process_event(watchEvent); // call event handle function
            buffPointer += sizeof(struct inotify_event) + watchEvent->len; // move buffer pointer
        }
    }

    return EXT_SUCCESS;
}
