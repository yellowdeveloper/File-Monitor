#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/inotify.h>
#include <libnotify/notify.h>
#include <unistd.h>
#include <pthread.h>                 // 추가 라이브러리
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <libconfig.h>

#define EXT_SUCCESS 0
#define EXT_ERR_TOO_FEW_ARGS 1
#define EXT_ERR_INIT_INOTIFY 2
#define EXT_ERR_ADD_WATCH 3
#define EXT_ERR_BASE_PATH_NULL 4
#define EXT_ERR_READ_INOTIFY 5
#define EXT_ERR_CONFIG_FILE 6

int IeventQueue = -1;
char* ProgramTitle = "file_monitor";
time_t lastEventTime = 0;            // 마지막 이벤트 발생 시간
FILE* logFile = NULL;                // 로그 파일 포인터
char logFilePath[512];               // 로그 파일 경로 (설정에서 읽음)
char filteredExtension[64] = "";     // 필터링할 확장자 (설정에서 읽음)

// 디렉토리와 wd (wd)의 매핑 테이블
typedef struct {
    int wd;                   // wd
    char path[512];                  // 디렉토리 경로
} WatchDescriptor;

WatchDescriptor watchDescriptors[512];  // wd 배열
int watchDescriptorCount = 0;           // 등록된 wd의 개수

// 로그 파일 초기화 함수
void init_log_file(const char* path) {
    logFile = fopen(path, "a");       // 로그 파일 열기
    if (!logFile) {
        perror("Error opening log file");  // 파일 열기 실패 시 오류 메시지 출력
        exit(EXIT_FAILURE);           // 종료
    }
    printf("Log file initialized at: %s\n", path);  // 로그 파일 초기화 완료 메시지 출력
}

// 로그 이벤트 함수
void log_event(const char* eventMessage) {
    if (logFile) {
        fprintf(logFile, "%s\n", eventMessage);  // 로그 파일에 메시지 기록
        fflush(logFile);                         // 버퍼 플러시
    }
    printf("%s\n", eventMessage);     // 콘솔 출력
}

// 설정 파일 읽기
void read_config(const char* configPath, char monitoredDirs[][512], int* dirCount) {
    config_t cfg;                     // libconfig 설정 객체
    config_init(&cfg);                // 설정 객체 초기화

    // 설정 파일
    if (!config_read_file(&cfg, configPath)) {  // 설정 파일 읽기
        fprintf(stderr, "Error reading config file %s: %s\n", configPath, config_error_text(&cfg));  // 설정 파일 읽기 오류
        config_destroy(&cfg);         // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE);    // 설정 파일 오류 시 종료
    }

    // 로그 파일
    const char* logPath = NULL;
    if (config_lookup_string(&cfg, "log_file", &logPath)) {  // 설정 파일에서 'log_file' 항목 읽기
        strncpy(logFilePath, logPath, sizeof(logFilePath));  // 로그 파일 경로 저장
    }
    else {
        fprintf(stderr, "Missing 'log_file' in config file\n");
        config_destroy(&cfg);         // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE);    // 로그 파일 경로가 없으면 종료
    }

    // 필터링 확장자
    const char* filterExt = NULL;
    if (config_lookup_string(&cfg, "filtered_extension", &filterExt)) {  // 필터링할 확장자 읽기
        strncpy(filteredExtension, filterExt, sizeof(filteredExtension));  // 필터링 확장자 저장
    }

    // 디렉토리
    config_setting_t* directories = config_lookup(&cfg, "monitor_directories");  // 디렉토리 목록 읽기
    if (directories) {
        if (config_setting_is_array(directories)) {  // 'monitor_directories'가 배열인지 확인
            int count = config_setting_length(directories);  // 배열의 길이 (디렉토리 개수)
            for (int i = 0; i < count && i < 512; ++i) {  // 디렉토리 경로를 배열에 저장
                const char* dir = config_setting_get_string_elem(directories, i);
                strncpy(monitoredDirs[i], dir, 512);  // 디렉토리 경로 저장
            }
            *dirCount = count;        // 디렉토리 개수 설정
        }
        else {
            fprintf(stderr, "'monitor_directories' must be an array in config file\n");
            config_destroy(&cfg);     // 설정 객체 해제
            exit(EXT_ERR_CONFIG_FILE);  // 배열이 아니면 종료
        }
    }
    else {
        fprintf(stderr, "Missing 'monitor_directories' in config file\n");
        config_destroy(&cfg);         // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE);    // 디렉토리가 없으면 종료
    }

    config_destroy(&cfg);             // 설정 객체 해제
}

// 디렉토리 감시 추가 함수
void add_watch_recursive(const char* path) {
    DIR* dir = opendir(path);         // 디렉토리 열기
    if (!dir) {
        perror("Error opening directory");
        return;                       // 리턴
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {  // 디렉토리 엔트리 읽기
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;                 // '.'과 '..'은 무시
        }

        // 하위 디렉토리
        char subPath[512];
        snprintf(subPath, sizeof(subPath), "%s/%s", path, entry->d_name);  // 하위 디렉토리 경로 생성

        struct stat pathStat;
        if (stat(subPath, &pathStat) == 0 && S_ISDIR(pathStat.st_mode)) {  // 디렉토리인지 확인
            add_watch_recursive(subPath);  // 하위 디렉토리 감시 추가
        }
    }

    int wd = inotify_add_watch(IeventQueue, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE_SELF);
    if (wd == -1) {
        fprintf(stderr, "Error adding watch for %s\n", path);
    }
    else {
        // wd와 디렉토리 매핑 저장
        strncpy(watchDescriptors[watchDescriptorCount].path, path, 512);  // 디렉토리 경로 저장
        watchDescriptors[watchDescriptorCount].wd = wd;  // wd 저장
        watchDescriptorCount++;       // wd 개수 증가

        printf("Watching: %s\n", path);  // 감시 디렉토리 출력
    }

    closedir(dir);                    // 디렉토리 닫기
}

// wd를 경로로 변환하는 함수 (watchMask 필드는 inotify_event 구조체에 존재하지 않으므로 수정)
const char* get_path_from_wd(int wd) {
    for (int i = 0; i < watchDescriptorCount; ++i) {
        if (watchDescriptors[i].wd == wd) {  // wd가 일치하면 경로 반환
            return watchDescriptors[i].path;
        }
    }
    return "Unknown path";  // 경로를 찾지 못한 경우
}

// 파일 확장자 필터링 함수
int has_filtered_extension(const char* filename) {
    if (strlen(filteredExtension) == 0) return 0;  // 필터링 확장자가 없는 경우 필터링하지 않음

    const char* ext = strrchr(filename, '.');  // 파일명에서 확장자를 추출
    if (ext != NULL && strcmp(ext + 1, filteredExtension) == 0) {
        return 1;           // 필터링 확장자와 일치하면 1 반환
    }
    return 0;               // 확장자가 일치하지 않으면 0 반환
}

// 필터링 확장자 확인 함수
void check_filtered_extension() {
    if (strlen(filteredExtension) > 0) {
        printf("Filtering files with extension: .%s\n", filteredExtension);  // 필터링 확장자 출력
    }
    else {
        printf("No extensions are filtered\n");  // 필터링 확장자가 없으면 출력
    }
}

// 이벤트 처리 함수
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) {
        const char* filename = watchEvent->name;

        // 필터링된 확장자일 경우 처리하지 않음
        if (has_filtered_extension(filename)) {
            return;         // 필터링된 파일은 이벤트를 처리하지 않음
        }

        char notificationMessage[1024];  // 이벤트 메시지 저장
        char fullPath[512]; // 파일의 전체 경로 저장
        char eventTime[64]; // 이벤트 발생 시간 저장
        time_t currentTime = time(NULL);  // 현재 시간 기록

        // 발생 시간 포맷
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));

        const char* basePath = get_path_from_wd(watchEvent->wd);  // wd에 해당하는 경로 얻기
        snprintf(fullPath, sizeof(fullPath), "%s/%s", basePath, watchEvent->name);  // 전체 경로 생성
        snprintf(notificationMessage, sizeof(notificationMessage), "[%s] File %s: ", eventTime, fullPath);

        if (watchEvent->mask & IN_CREATE) {
            strcat(notificationMessage, "created");
        }
        else if (watchEvent->mask & IN_DELETE) {
            strcat(notificationMessage, "deleted");
        }
        else if (watchEvent->mask & IN_MODIFY) {
            strcat(notificationMessage, "modified");
        }
        else if (watchEvent->mask & IN_MOVE_SELF) {
            strcat(notificationMessage, "moved");
        }

        // 마지막 이벤트가 1초 이상 간격을 두고 발생한 경우 로그 기록
        if (difftime(currentTime, lastEventTime) >= 1) {
            lastEventTime = currentTime;  // 마지막 이벤트 시간 갱신
            log_event(notificationMessage);  // 로그에 이벤트 기록
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor CONFIG_PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    char monitoredDirs[512][512];  // 모니터링할 디렉토리 배열
    int dirCount = 0;       // 모니터링할 디렉토리 개수

    read_config(argv[1], monitoredDirs, &dirCount);  // 설정 파일 읽기
    init_log_file(logFilePath);  // 로그 파일 초기화

    check_filtered_extension();  // 필터링 확장자 확인

    IeventQueue = inotify_init();
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    for (int i = 0; i < dirCount; ++i) {
        add_watch_recursive(monitoredDirs[i]);  // 디렉토리 감시 추가
    }

    char buffer[4096];
    while (1) {
        int readLength = read(IeventQueue, buffer, sizeof(buffer));
        if (readLength == -1) {
            fprintf(stderr, "Error reading from inotify instance\n");
            exit(EXT_ERR_READ_INOTIFY);
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength;) {
            const struct inotify_event* watchEvent = (const struct inotify_event*)buffPointer;
            process_event(watchEvent);
            buffPointer += sizeof(struct inotify_event) + watchEvent->len;
        }
    }

    return EXT_SUCCESS;
}