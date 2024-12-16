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
#include <limits.h>
#include <canberra.h>

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

// gtk variables
GtkWidget *logWindow;
GtkWidget *logTextView;
GtkTextBuffer *logBuffer;
GtkWidget *directoryListBox;
GtkWidget *directoryContentsBox;
GtkWidget *selectedDirectoryBox = NULL;


// 디렉토리와 watch descriptor (wd)의 매핑 테이블
typedef struct {
    int wd;                           // watch descriptor
    char path[512];                   // 디렉토리 경로

    GtkWidget *eventBox;
} WatchDescriptor;

WatchDescriptor watchDescriptors[512];  // watch descriptor 배열
int watchDescriptorCount = 0;           // 등록된 watch descriptor의 개수

void event_sound() {
    ca_context *context = NULL;

    // libcanberra 컨텍스트 초기화
    if (ca_context_create(&context) < 0) {
        fprintf(stderr, "Failed to create sound context\n");
        return;
    }

    // 간단한 이벤트 사운드 재생 (기본 사운드 테마)
    ca_context_play(context, 0,
                    CA_PROP_EVENT_ID, "message-new-instant",
                    CA_PROP_EVENT_DESCRIPTION, "File event notification",
                    NULL);

    // 컨텍스트 해제
    ca_context_destroy(context);
}

void initialize_css() {
    const char *css = 
        ".selected { background-color: #d1ecf1; border: 1px solid #0c5460; }"
        ".highlighted { background-color: #ffcccc; border: 1px solid #cc0000; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);

    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

void apply_custom_css(GtkWidget *widget, const char *css) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);

    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_USER);

    g_object_unref(provider);
}

void on_directory_clicked(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) { // 클릭 감지
        if (selectedDirectoryBox) {
            // 이전에 선택된 디렉토리 상자의 스타일 제거
            GtkStyleContext *prevContext = gtk_widget_get_style_context(selectedDirectoryBox);
            gtk_style_context_remove_class(prevContext, "selected");
        }

        // 새로 선택된 디렉토리 상자에 스타일 추가
        GtkStyleContext *currentContext = gtk_widget_get_style_context(widget);
        gtk_style_context_add_class(currentContext, "selected");

        selectedDirectoryBox = widget; // 선택된 디렉토리 상자 업데이트
    }
}

void on_directory_double_click(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) { // 더블 클릭 감지
        const char *clickedPath = (const char *)data;

        // 클릭된 디렉토리의 절대 경로를 생성
        char absolutePath[PATH_MAX];
        realpath(clickedPath, absolutePath);

        printf("Double-clicked directory: %s\n", absolutePath); // 디버깅 출력

        GtkStyleContext *context = gtk_widget_get_style_context(widget);
        if (gtk_style_context_has_class(context, "highlighted")) {
            gtk_style_context_remove_class(context, "highlighted");
        }
        
        // 디렉토리 내용을 표시
        show_directory_contents(absolutePath);
    }
}

void show_directory_contents(const char *directory) {
    // 기존 내용 삭제
    GList *children = gtk_container_get_children(GTK_CONTAINER(directoryContentsBox));
    for (GList *iter = children; iter != NULL; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    // 디렉토리 열기
    DIR *dir = opendir(directory);
    if (!dir) {
        perror("Error opening directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue; // 현재 디렉토리와 부모 디렉토리는 무시
        }

        // 항목의 전체 경로 생성
        char fullPath[PATH_MAX];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", directory, entry->d_name);

        // 레이블 생성
        GtkWidget *eventBox = gtk_event_box_new();
        GtkWidget *label = gtk_label_new(entry->d_name);

        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(eventBox), label);

        // 더블 클릭 이벤트 연결
        char *pathCopy = strdup(fullPath); // 경로를 복사해 이벤트 핸들러로 전달
        gtk_widget_add_events(eventBox, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(eventBox, "button-press-event", G_CALLBACK(on_directory_double_click), pathCopy);

        gtk_container_add(GTK_CONTAINER(directoryContentsBox), eventBox);
        gtk_widget_show_all(eventBox);
    }

    closedir(dir);
}

// 디렉토리 목록에 아이템 추가
void add_directory_to_list(const char *directory) {
    GtkWidget *eventBox = gtk_event_box_new();
    GtkWidget *label = gtk_label_new(directory);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(eventBox), label);

    // 더블 클릭 이벤트 연결
    char *pathCopy = strdup(directory); // 경로 복사
    gtk_widget_add_events(eventBox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(eventBox, "button-press-event", G_CALLBACK(on_directory_double_click), pathCopy);

    // 클릭 이벤트 연결
    g_signal_connect(eventBox, "button-press-event", G_CALLBACK(on_directory_clicked), NULL);

    gtk_container_add(GTK_CONTAINER(directoryListBox), eventBox);
    gtk_widget_show_all(eventBox);

    for (int i = 0; i < watchDescriptorCount; ++i) {
        if (strcmp(watchDescriptors[i].path, directory) == 0) {
            watchDescriptors[i].eventBox = eventBox;
            break;
        }
    }
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
    gtk_window_set_default_size(GTK_WINDOW(logWindow), 1000, 600);

    // === 상단 영역: 디렉토리 목록 ===
    GtkWidget *directoryHeader = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(directoryHeader), "Monitoring Directories");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(directoryHeader), FALSE);

    directoryListBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *topScrollWindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(topScrollWindow), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(topScrollWindow), directoryListBox);

    GtkWidget *topFrame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(topFrame), directoryHeader, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topFrame), topScrollWindow, TRUE, TRUE, 0);

    // === 오른쪽 영역: 디렉토리 내용 ===
    GtkWidget *contentsHeader = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(contentsHeader), "Directory Contents");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(contentsHeader), FALSE);

    directoryContentsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *contentsScrollWindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(contentsScrollWindow), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(contentsScrollWindow), directoryContentsBox);

    GtkWidget *contentsFrame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(contentsFrame), contentsHeader, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentsFrame), contentsScrollWindow, TRUE, TRUE, 0);

    // === 하단 영역: 로그 창 ===
    GtkWidget *logHeader = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(logHeader), "Event Logs");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(logHeader), FALSE);

    logTextView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logTextView), FALSE);
    logBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logTextView));

    GtkWidget *logScrollWindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(logScrollWindow), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(logScrollWindow), logTextView);

    GtkWidget *bottomFrame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(bottomFrame), logHeader, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottomFrame), logScrollWindow, TRUE, TRUE, 0);

    // === 전체 레이아웃 ===
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); // 상단+오른쪽 수평 박스
    gtk_box_pack_start(GTK_BOX(hbox), topFrame, TRUE, TRUE, 0);   // 상단 영역
    gtk_box_pack_start(GTK_BOX(hbox), contentsFrame, TRUE, TRUE, 0); // 오른쪽 영역

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); // 수직 레이아웃
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);     // 상단+오른쪽
    gtk_box_pack_end(GTK_BOX(vbox), bottomFrame, TRUE, TRUE, 0); // 하단 로그

    gtk_container_add(GTK_CONTAINER(logWindow), vbox);

    // 창 닫기 이벤트 처리
    g_signal_connect(logWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(logWindow);
}

gboolean update_ui(gpointer data) {
    const char* eventMessage = (const char*)data;
    GtkTextIter endIter;

    // 텍스트 버퍼의 끝에 메시지 추가
    gtk_text_buffer_get_end_iter(logBuffer, &endIter);
    gtk_text_buffer_insert(logBuffer, &endIter, eventMessage, -1);
    gtk_text_buffer_insert(logBuffer, &endIter, "\n", -1);

    free(data);

    return FALSE;
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
        if (stat(subPath, &pathStat) == 0 && S_ISDIR(pathStat.st_mode)) { // 하위 디렉토리 확인
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

        for (int i = 0; i < watchDescriptorCount; ++i) {
            if (strcmp(watchDescriptors[i].path, basePath) == 0 && watchDescriptors[i].eventBox) {
                GtkStyleContext *context = gtk_widget_get_style_context(watchDescriptors[i].eventBox);
                gtk_style_context_add_class(context, "highlighted");
                break;
            }
        }

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

            event_sound();
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
    initialize_css();

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