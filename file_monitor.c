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
#include <gtk/gtk.h>

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
GtkTextBuffer *log_buffer = NULL; // log text buffer

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
    printf("Log file created/opened at: %s\n", logPath); // ����� �޽���
}

// event logging function
void log_event(const char* eventMessage) {
    if (log_buffer) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(log_buffer, &end);
        gtk_text_buffer_insert(log_buffer, &end, eventMessage, -1);
        gtk_text_buffer_insert(log_buffer, &end, "\n", -1);
    }
    
    if (logFile) {
         fprintf(logFile, "Event: %s\n", eventMessage); 
        fflush(logFile);
    }
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

void* inotify_thread(void* arg) {
    char buffer[4096];
    const struct inotify_event* watchEvent;

    while (1) {
        int readLength = read(IeventQueue, buffer, sizeof(buffer));
        if (readLength == -1) {
            perror("Error reading inotify instance");
            break;
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength; ) {
            watchEvent = (const struct inotify_event*)buffPointer;
            process_event(watchEvent);
            buffPointer += sizeof(struct inotify_event) + watchEvent->len;
        }
    }
    return NULL;
}

void on_destroy(GtkWidget* widget, gpointer data) {
    if (IeventQueue != -1) {
        close(IeventQueue);
    }
    gtk_main_quit();
}

int main(int argc, char** argv) {
    char* basePath = NULL;

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

    // initialize gtk
    gtk_init(&argc, &argv);

    // create main window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "File Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    // view text logging
    GtkWidget* scrolledWindow = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* textView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textView), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolledWindow), textView);
    gtk_container_add(GTK_CONTAINER(window), scrolledWindow);

    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));

    // start inotify event handling thread
    pthread_t thread;
    if (pthread_create(&thread, NULL, inotify_thread, NULL) != 0) {
        perror("Error creating inotify thread");
        return EXIT_FAILURE;
    }

    // gtk main loop
    gtk_widget_show_all(window);
    gtk_main();

    // clean up
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    if(logFile) fclose(logFile);

    return EXT_SUCCESS;
}
