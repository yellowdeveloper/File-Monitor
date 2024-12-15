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
#include <glib.h>

#define EXT_SUCCESS 0                // 성공 코드
#define EXT_ERR_TOO_FEW_ARGS 1       // 인자 부족 오류 코드
#define EXT_ERR_INIT_INOTIFY 2       // inotify 초기화 실패 오류 코드
#define EXT_ERR_ADD_WATCH 3          // 디렉토리 감시 추가 실패 오류 코드
#define EXT_ERR_READ_INOTIFY 5       // inotify 이벤트 읽기 오류 코드
#define EXT_ERR_CONFIG_FILE 6        // 설정 파일 읽기 오류 코드

// 전역 변수들
int IeventQueue = -1;                // inotify 대기 큐 (이벤트를 기다리는 큐)
char* ProgramTitle = "file_monitor"; // 프로그램 제목
time_t lastEventTime = 0;            // 마지막 이벤트 발생 시간
FILE* logFile = NULL;                // 로그 파일 포인터
char logFilePath[512];               // 로그 파일 경로 (설정에서 읽음)
char filteredExtension[64] = "";     // 필터링할 확장자 (설정에서 읽음)

GtkWidget *logWindow;    // 로그 출력용 창
GtkWidget *logTextView;  // 로그 출력용 텍스트 뷰
GtkTextBuffer *logBuffer; // 텍스트 버퍼
GtkWidget *directoryListBox;

// 디렉토리와 watch descriptor (wd)의 매핑 테이블
typedef struct {
    int wd;                           // watch descriptor
    char path[512];                   // 디렉토리 경로
} WatchDescriptor;

WatchDescriptor watchDescriptors[512];  // watch descriptor 배열
int watchDescriptorCount = 0;           // 등록된 watch descriptor의 개수

void add_directory_to_list(const char *directory) {
    GtkWidget *label = gtk_label_new(directory); // 디렉토리를 레이블로 표시
    gtk_widget_set_halign(label, GTK_ALIGN_START); // 왼쪽 정렬
    gtk_container_add(GTK_CONTAINER(directoryListBox), label); // 리스트 박스에 추가
    gtk_widget_show(label); // 레이블 표시
}

// 로그 파일 초기화 함수
void init_log_file(const char* path) {
    logFile = fopen(path, "a"); // 로그 파일 열기 (추가 모드)
    if (!logFile) {
        perror("Error opening log file"); // 파일 열기 실패 시 오류 메시지 출력
        exit(EXIT_FAILURE); // 프로그램 종료
    }
    printf("Log file initialized at: %s\n", path); // 로그 파일 초기화 완료 메시지 출력
}

void init_log_ui() {
    gtk_init(NULL, NULL);

    // 메인 윈도우 생성
    logWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(logWindow), "File Monitor Logs");
    gtk_window_set_default_size(GTK_WINDOW(logWindow), 800, 600);

    // 상단에 디렉토리 목록을 보여줄 박스 생성
    GtkWidget *topLabel = gtk_label_new("Monitoring Directories:");
    gtk_widget_set_halign(topLabel, GTK_ALIGN_START);
    gtk_widget_set_margin_top(topLabel, 10);

    directoryListBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); // 수직 박스
    GtkWidget *topArea = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); // 상단 영역
    gtk_box_pack_start(GTK_BOX(topArea), topLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topArea), directoryListBox, TRUE, TRUE, 0);

    // 하단에 로그 출력을 위한 텍스트 뷰 생성
    logTextView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logTextView), FALSE); // 읽기 전용
    logBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logTextView));

    // 텍스트 뷰를 스크롤 가능한 창에 추가
    GtkWidget *scrollWindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollWindow), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrollWindow), logTextView);

    // 전체 레이아웃 구성
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); // 수직 레이아웃 박스
    gtk_box_pack_start(GTK_BOX(vbox), topArea, FALSE, FALSE, 0); // 상단 영역 추가
    gtk_box_pack_end(GTK_BOX(vbox), scrollWindow, TRUE, TRUE, 0); // 하단 로그 창 추가

    gtk_container_add(GTK_CONTAINER(logWindow), vbox); // 메인 윈도우에 레이아웃 추가

    // 창 닫기 이벤트 처리
    g_signal_connect(logWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(logWindow); // 모든 위젯 표시
}

gboolean update_ui(gpointer data) {
    const char* eventMessage = (const char*)data;
    GtkTextIter endIter;

    // 텍스트 버퍼의 끝에 메시지 추가
    gtk_text_buffer_get_end_iter(logBuffer, &endIter);
    gtk_text_buffer_insert(logBuffer, &endIter, eventMessage, -1);
    gtk_text_buffer_insert(logBuffer, &endIter, "\n", -1);

    // 동적 메모리 해제
    free(data);

    return FALSE; // 작업 완료 후 다시 호출하지 않음
}


// 로그 이벤트 함수
void log_event(const char* eventMessage) {
    if (!eventMessage || strlen(eventMessage) == 0) {
        fprintf(stderr, "Invalid event message\n");
        return;
    }

    // 메시지를 복사하여 GTK 메인 스레드에 전달
    char* messageCopy = strdup(eventMessage);
    g_idle_add(update_ui, messageCopy);

    // 콘솔에도 출력 (디버깅 용도)
    printf("%s\n", eventMessage);
}

// 설정 파일에서 디렉토리 및 로그 파일 경로 읽기
void read_config(const char* configPath, char monitoredDirs[][512], int* dirCount) {
    config_t cfg; // libconfig 설정 객체
    config_init(&cfg); // 설정 객체 초기화

    if (!config_read_file(&cfg, configPath)) {  // 설정 파일 읽기
        fprintf(stderr, "Error reading config file %s: %s\n", configPath, config_error_text(&cfg));
        config_destroy(&cfg);  // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE); // 설정 파일 오류 시 종료
    }

    const char* logPath = NULL;
    if (config_lookup_string(&cfg, "log_file", &logPath)) { // 설정 파일에서 'log_file' 항목 읽기
        strncpy(logFilePath, logPath, sizeof(logFilePath));  // 로그 파일 경로 저장
    }
    else {
        fprintf(stderr, "Missing 'log_file' in config file\n");
        config_destroy(&cfg); // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE); // 로그 파일 경로가 없으면 종료
    }

    const char* filterExt = NULL;
    if (config_lookup_string(&cfg, "filtered_extension", &filterExt)) { // 필터링할 확장자 읽기
        strncpy(filteredExtension, filterExt, sizeof(filteredExtension)); // 필터링 확장자 저장
    }

    config_setting_t* directories = config_lookup(&cfg, "monitor_directories"); // 디렉토리 목록 읽기
    if (directories) {
        if (config_setting_is_array(directories)) { // 'monitor_directories'가 배열인지 확인
            int count = config_setting_length(directories); // 배열의 길이 (디렉토리 개수)
            for (int i = 0; i < count && i < 512; ++i) { // 디렉토리 경로를 배열에 저장
                const char* dir = config_setting_get_string_elem(directories, i);
                strncpy(monitoredDirs[i], dir, 512); // 디렉토리 경로 저장
            }
            *dirCount = count; // 디렉토리 개수 설정
        }
        else {
            fprintf(stderr, "'monitor_directories' must be an array in config file\n");
            config_destroy(&cfg); // 설정 객체 해제
            exit(EXT_ERR_CONFIG_FILE); // 배열이 아니면 종료
        }
    }
    else {
        fprintf(stderr, "Missing 'monitor_directories' in config file\n");
        config_destroy(&cfg); // 설정 객체 해제
        exit(EXT_ERR_CONFIG_FILE); // 디렉토리가 없으면 종료
    }

    config_destroy(&cfg); // 설정 객체 해제
}

// 디렉토리 감시 추가 함수 (하위 디렉토리도 포함)
void add_watch_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        perror("Error opening directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subPath[512];
        snprintf(subPath, sizeof(subPath), "%s/%s", path, entry->d_name);

        struct stat pathStat;
        if (stat(subPath, &pathStat) == 0 && S_ISDIR(pathStat.st_mode)) {
            add_watch_recursive(subPath);
        }
    }

    int wd = inotify_add_watch(IeventQueue, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE_SELF);
    if (wd == -1) {
        fprintf(stderr, "Error adding watch for %s\n", path);
    } else {
        strncpy(watchDescriptors[watchDescriptorCount].path, path, 512);
        watchDescriptors[watchDescriptorCount].wd = wd;
        watchDescriptorCount++;

        printf("Watching: %s\n", path); // 콘솔에 출력
        add_directory_to_list(path); // 디렉토리 목록에 추가
    }

    closedir(dir);
}

// watch descriptor를 경로로 변환하는 함수
const char* get_path_from_wd(int wd) {
    for (int i = 0; i < watchDescriptorCount; ++i) {
        if (watchDescriptors[i].wd == wd) { // watch descriptor가 일치하면 경로 반환
            return watchDescriptors[i].path;
        }
    }
    return "Unknown path"; // 경로를 찾지 못한 경우
}

// 파일 확장자 필터링 함수
int has_filtered_extension(const char* filename) {
    if (strlen(filteredExtension) == 0) return 0;  // 필터링할 확장자가 없으면 모두 허용

    const char* ext = strrchr(filename, '.');  // 파일 확장자 찾기
    if (ext != NULL && strcmp(ext + 1, filteredExtension) == 0) {
        return 1;  // 필터링 확장자와 일치하면 1 반환
    }

    return 0;  // 확장자가 일치하지 않으면 0 반환
}

// 필터링 확장자 확인 함수 (한 번만 출력)
void check_filtered_extension() {
    if (strlen(filteredExtension) > 0) {
        printf("Filtering files with extension: .%s\n", filteredExtension); // 필터링 확장자 출력
    }
    else {
        printf("No extensions are filtered\n"); // 필터링 확장자가 없으면 출력
    }
}

// 이벤트 처리 함수
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) {
        const char* filename = watchEvent->name;

        // 필터링된 확장자일 경우 처리하지 않음
        if (has_filtered_extension(filename)) {
            return; // 필터링된 파일은 이벤트를 처리하지 않음
        }

        char notificationMessage[1024]; // 이벤트 메시지 저장
        char fullPath[512]; // 파일의 전체 경로 저장
        char eventTime[64]; // 이벤트 발생 시간 저장
        time_t currentTime = time(NULL); // 현재 시간 얻기

        // 발생 시간 포맷팅
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));

        const char* basePath = get_path_from_wd(watchEvent->wd); // watch descriptor에 해당하는 경로 얻기
        snprintf(fullPath, sizeof(fullPath), "%s/%s", basePath, watchEvent->name); // 전체 경로 생성
        snprintf(notificationMessage, sizeof(notificationMessage), "[%s] File %s: ", eventTime, fullPath);

        // 이벤트 종류에 따라 메시지 작성
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
            lastEventTime = currentTime; // 마지막 이벤트 시간 갱신
            log_event(notificationMessage); // 로그에 이벤트 기록
        }
    }
}

void* inotify_thread(void* arg) {
    char buffer[4096]; // 이벤트를 받을 버퍼

    while (1) {
        int readLength = read(IeventQueue, buffer, sizeof(buffer)); // inotify 이벤트 읽기
        if (readLength == -1) {
            fprintf(stderr, "Error reading from inotify instance\n");
            exit(EXT_ERR_READ_INOTIFY); // 이벤트 읽기 실패 시 종료
        }

        for (char* buffPointer = buffer; buffPointer < buffer + readLength;) {
            const struct inotify_event* watchEvent = (const struct inotify_event*)buffPointer; // 이벤트 처리
            process_event(watchEvent);
            buffPointer += sizeof(struct inotify_event) + watchEvent->len; // 버퍼 이동
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { // 인자가 부족한 경우
        fprintf(stderr, "USAGE: file_monitor CONFIG_PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS); // 종료
    }

    char monitoredDirs[512][512];  // 모니터링할 디렉토리 배열
    int dirCount = 0; // 모니터링할 디렉토리 개수

    read_config(argv[1], monitoredDirs, &dirCount); // 설정 파일 읽기
    init_log_file(logFilePath); // 로그 파일 초기화
    init_log_ui();

    check_filtered_extension();  // 필터링 확장자 확인 (한 번만 출력)

    IeventQueue = inotify_init();  // inotify 인스턴스 초기화
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY); // 초기화 실패 시 종료
    }

    for (int i = 0; i < dirCount; ++i) {
        add_watch_recursive(monitoredDirs[i]); // 디렉토리 감시 추가
    }

    pthread_t thread;
    pthread_create(&thread, NULL, inotify_thread, NULL);

    gtk_main();

    return EXT_SUCCESS; // 정상 종료
}