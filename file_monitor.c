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

// ���� �ڵ� ����
#define EXT_SUCCESS 0
#define EXT_ERR_TOO_FEW_ARGS 1
#define EXT_ERR_INIT_INOTIFY 2
#define EXT_ERR_ADD_WATCH 3
#define EXT_ERR_BASE_PATH_NULL 4
#define EXT_ERR_READ_INOTIFY 5
#define EXT_ERR_LIBNOTIFY 6

// ���� ������
int IeventStatus = -1; // inotify ���� ����
int IeventQueue = -1; // inotify ��� ť
char* ProgramTitle = "file_monitor"; // ���α׷� ����
FILE* logFile = NULL; // �α� ���� ������
const char* filteredExtension = NULL; // ���͸��� ���� Ȯ����
int emailEnabled = 0; // �̸��� �˸� Ȱ��ȭ ����
const char* emailRecipient = NULL; // �̸��� ������
const char* logFilePath = NULL; // �α� ���� ���
time_t lastEventTime = 0; // ������ �̺�Ʈ �߻� �ð�

// �α� ���� ũ�� üũ �� ��ȯ ó�� �Լ�
void rotate_log_file(const char* logPath) {
    if (logFile) {
        fclose(logFile);
    }

    // �� �α� ���� ����
    logFile = fopen(logPath, "a");
    if (!logFile) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    printf("Log file created/opened at: %s\n", logPath); // ����� �޽���
}

// �ñ׳� ó�� �Լ�: ���α׷� ���� �� ���� �۾�
void signal_handler(int signal) {
    printf("Signal received: cleaning up\n");

    // inotify ���� ����
    if (inotify_rm_watch(IeventQueue, IeventStatus) == -1) {
        fprintf(stderr, "Error removing from watch queue!\n");
    }
    close(IeventQueue); // inotify ��� ť ����

    // �α� ���� �ݱ�
    if (logFile) {
        fclose(logFile);
    }

    exit(EXIT_SUCCESS);
}

// �̺�Ʈ�� �α� ���Ͽ� ����ϴ� �Լ�
void log_event(const char* eventMessage) {
    if (logFile) {
        fprintf(logFile, "Event: %s\n", eventMessage); // �̺�Ʈ �޽��� ���
        fflush(logFile); // ���� ���ۿ� ���
    }
}

// ������ �α� ���Ͽ� ����ϴ� �Լ�
void log_error(const char* message) {
    if (logFile) {
        fprintf(logFile, "Error: %s\n", message); // ���� �޽��� ���
        fflush(logFile); // ���� ���ۿ� ���
    }
}

// �̺�Ʈ ó�� �Լ�
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) { // ���� �̸��� ���� ��쿡�� ó��
        char notificationMessage[512];
        char eventTime[64];
        time_t currentTime = time(NULL);

        // �߻� �ð� ������
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));
        snprintf(notificationMessage, sizeof(notificationMessage), "[%s] File %s: ", eventTime, watchEvent->name); // �ð� ���� �޽��� ����

        // �̺�Ʈ �������� �޽��� �߰�
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
            strcat(notificationMessage, "unknown event"); // ��Ÿ �̺�Ʈ ó��
        }

        // �̺�Ʈ�� 1�� �������� ó���ǵ��� �ð� ��
        if (difftime(currentTime, lastEventTime) >= 1) {  // �ּ� 1�� �������� �̺�Ʈ ó��
            lastEventTime = currentTime;

            // �̺�Ʈ �α� ���
            log_event(notificationMessage);

            // �ֿܼ� ���
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

    const uint32_t watchMask = IN_CREATE | IN_DELETE | IN_ACCESS | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF; // ������ �̺�Ʈ ���

    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    // ���� ���� �б� �� �α� ���� �ʱ�ȭ
    logFilePath = "file_monitor.log"; // �α� ���� ��� ���� (�⺻��)

    rotate_log_file(logFilePath); // �α� ���� �ʱ�ȭ

    basePath = (char*)malloc(sizeof(char) * (strlen(argv[1]) + 1));
    strcpy(basePath, argv[1]); // ����͸��� ���丮 ��� ����

    IeventQueue = inotify_init(); // inotify �ʱ�ȭ
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    IeventStatus = inotify_add_watch(IeventQueue, argv[1], watchMask); // ���丮 ���� �߰�
    if (IeventStatus == -1) {
        fprintf(stderr, "Error adding file to watchlist\n");
        exit(EXT_ERR_ADD_WATCH);
    }

    while (1) {
        if (!eventDetected) {
            printf("waiting for event...\n");
            eventDetected = 1;
        }

        readLength = read(IeventQueue, buffer, sizeof(buffer)); // inotify���� �̺�Ʈ �б�
        if (readLength == -1) {
            fprintf(stderr, "Error reading from inotify instance\n");
            exit(EXT_ERR_READ_INOTIFY);
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength; ) {
            watchEvent = (const struct inotify_event*)buffPointer;
            process_event(watchEvent); // �̺�Ʈ ó�� �Լ� ȣ��
            buffPointer += sizeof(struct inotify_event) + watchEvent->len; // ���� ������ �̵�
        }
    }

    return EXT_SUCCESS;
}