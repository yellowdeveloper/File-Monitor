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

// 에러 코드 정의
#define EXT_SUCCESS 0
#define EXT_ERR_TOO_FEW_ARGS 1
#define EXT_ERR_INIT_INOTIFY 2
#define EXT_ERR_ADD_WATCH 3
#define EXT_ERR_BASE_PATH_NULL 4
#define EXT_ERR_READ_INOTIFY 5
#define EXT_ERR_LIBNOTIFY 6

// 전역 변수들
int IeventStatus = -1; // inotify 감시 상태
int IeventQueue = -1; // inotify 대기 큐
char* ProgramTitle = "file_monitor"; // 프로그램 제목
FILE* logFile = NULL; // 로그 파일 포인터
const char* filteredExtension = NULL; // 필터링할 파일 확장자
int emailEnabled = 0; // 이메일 알림 활성화 여부
const char* emailRecipient = NULL; // 이메일 수신자
const char* logFilePath = NULL; // 로그 파일 경로
time_t lastEventTime = 0; // 마지막 이벤트 발생 시간

// 로그 파일 크기 체크 및 순환 처리 함수
void rotate_log_file(const char* logPath) {
    if (logFile) {
        fclose(logFile);
    }

    // 새 로그 파일 열기
    logFile = fopen(logPath, "a");
    if (!logFile) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    printf("Log file created/opened at: %s\n", logPath); // 디버깅 메시지
}

// 시그널 처리 함수: 프로그램 종료 시 정리 작업
void signal_handler(int signal) {
    printf("Signal received: cleaning up\n");

    // inotify 감시 제거
    if (inotify_rm_watch(IeventQueue, IeventStatus) == -1) {
        fprintf(stderr, "Error removing from watch queue!\n");
    }
    close(IeventQueue); // inotify 대기 큐 종료

    // 로그 파일 닫기
    if (logFile) {
        fclose(logFile);
    }

    exit(EXIT_SUCCESS);
}

// 이벤트를 로그 파일에 기록하는 함수
void log_event(const char* eventMessage) {
    if (logFile) {
        fprintf(logFile, "Event: %s\n", eventMessage); // 이벤트 메시지 기록
        fflush(logFile); // 파일 버퍼에 기록
    }
}

// 에러를 로그 파일에 기록하는 함수
void log_error(const char* message) {
    if (logFile) {
        fprintf(logFile, "Error: %s\n", message); // 에러 메시지 기록
        fflush(logFile); // 파일 버퍼에 기록
    }
}

// 이벤트 처리 함수
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) { // 파일 이름이 있을 경우에만 처리
        char notificationMessage[512];
        char eventTime[64];
        time_t currentTime = time(NULL);

        // 발생 시간 포맷팅
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));
        snprintf(notificationMessage, sizeof(notificationMessage), "[%s] File %s: ", eventTime, watchEvent->name); // 시간 포함 메시지 생성

        // 이벤트 유형별로 메시지 추가
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
            strcat(notificationMessage, "unknown event"); // 기타 이벤트 처리
        }

        // 이벤트가 1초 간격으로 처리되도록 시간 비교
        if (difftime(currentTime, lastEventTime) >= 1) {  // 최소 1초 간격으로 이벤트 처리
            lastEventTime = currentTime;

            // 이벤트 로그 기록
            log_event(notificationMessage);

            // 콘솔에 출력
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

    const uint32_t watchMask = IN_CREATE | IN_DELETE | IN_ACCESS | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF; // 감시할 이벤트 목록

    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    // 설정 파일 읽기 및 로그 파일 초기화
    logFilePath = "file_monitor.log"; // 로그 파일 경로 설정 (기본값)

    rotate_log_file(logFilePath); // 로그 파일 초기화

    basePath = (char*)malloc(sizeof(char) * (strlen(argv[1]) + 1));
    strcpy(basePath, argv[1]); // 모니터링할 디렉토리 경로 설정

    IeventQueue = inotify_init(); // inotify 초기화
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    IeventStatus = inotify_add_watch(IeventQueue, argv[1], watchMask); // 디렉토리 감시 추가
    if (IeventStatus == -1) {
        fprintf(stderr, "Error adding file to watchlist\n");
        exit(EXT_ERR_ADD_WATCH);
    }

    while (1) {
        if (!eventDetected) {
            printf("waiting for event...\n");
            eventDetected = 1;
        }

        readLength = read(IeventQueue, buffer, sizeof(buffer)); // inotify에서 이벤트 읽기
        if (readLength == -1) {
            fprintf(stderr, "Error reading from inotify instance\n");
            exit(EXT_ERR_READ_INOTIFY);
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength; ) {
            watchEvent = (const struct inotify_event*)buffPointer;
            process_event(watchEvent); // 이벤트 처리 함수 호출
            buffPointer += sizeof(struct inotify_event) + watchEvent->len; // 버퍼 포인터 이동
        }
    }

    return EXT_SUCCESS;
}