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
#include <pthread.h>                 // �߰� ���̺귯��
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
time_t lastEventTime = 0;            // ������ �̺�Ʈ �߻� �ð�
FILE* logFile = NULL;                // �α� ���� ������
char logFilePath[512];               // �α� ���� ��� (�������� ����)
char filteredExtension[64] = "";     // ���͸��� Ȯ���� (�������� ����)

// ���丮�� wd (wd)�� ���� ���̺�
typedef struct {
    int wd;                   // wd
    char path[512];                  // ���丮 ���
} WatchDescriptor;

WatchDescriptor watchDescriptors[512];  // wd �迭
int watchDescriptorCount = 0;           // ��ϵ� wd�� ����

// �α� ���� �ʱ�ȭ �Լ�
void init_log_file(const char* path) {
    logFile = fopen(path, "a");       // �α� ���� ����
    if (!logFile) {
        perror("Error opening log file");  // ���� ���� ���� �� ���� �޽��� ���
        exit(EXIT_FAILURE);           // ����
    }
    printf("Log file initialized at: %s\n", path);  // �α� ���� �ʱ�ȭ �Ϸ� �޽��� ���
}

// �α� �̺�Ʈ �Լ�
void log_event(const char* eventMessage) {
    if (logFile) {
        fprintf(logFile, "%s\n", eventMessage);  // �α� ���Ͽ� �޽��� ���
        fflush(logFile);                         // ���� �÷���
    }
    printf("%s\n", eventMessage);     // �ܼ� ���
}

// ���� ���� �б�
void read_config(const char* configPath, char monitoredDirs[][512], int* dirCount) {
    config_t cfg;                     // libconfig ���� ��ü
    config_init(&cfg);                // ���� ��ü �ʱ�ȭ

    // ���� ����
    if (!config_read_file(&cfg, configPath)) {  // ���� ���� �б�
        fprintf(stderr, "Error reading config file %s: %s\n", configPath, config_error_text(&cfg));  // ���� ���� �б� ����
        config_destroy(&cfg);         // ���� ��ü ����
        exit(EXT_ERR_CONFIG_FILE);    // ���� ���� ���� �� ����
    }

    // �α� ����
    const char* logPath = NULL;
    if (config_lookup_string(&cfg, "log_file", &logPath)) {  // ���� ���Ͽ��� 'log_file' �׸� �б�
        strncpy(logFilePath, logPath, sizeof(logFilePath));  // �α� ���� ��� ����
    }
    else {
        fprintf(stderr, "Missing 'log_file' in config file\n");
        config_destroy(&cfg);         // ���� ��ü ����
        exit(EXT_ERR_CONFIG_FILE);    // �α� ���� ��ΰ� ������ ����
    }

    // ���͸� Ȯ����
    const char* filterExt = NULL;
    if (config_lookup_string(&cfg, "filtered_extension", &filterExt)) {  // ���͸��� Ȯ���� �б�
        strncpy(filteredExtension, filterExt, sizeof(filteredExtension));  // ���͸� Ȯ���� ����
    }

    // ���丮
    config_setting_t* directories = config_lookup(&cfg, "monitor_directories");  // ���丮 ��� �б�
    if (directories) {
        if (config_setting_is_array(directories)) {  // 'monitor_directories'�� �迭���� Ȯ��
            int count = config_setting_length(directories);  // �迭�� ���� (���丮 ����)
            for (int i = 0; i < count && i < 512; ++i) {  // ���丮 ��θ� �迭�� ����
                const char* dir = config_setting_get_string_elem(directories, i);
                strncpy(monitoredDirs[i], dir, 512);  // ���丮 ��� ����
            }
            *dirCount = count;        // ���丮 ���� ����
        }
        else {
            fprintf(stderr, "'monitor_directories' must be an array in config file\n");
            config_destroy(&cfg);     // ���� ��ü ����
            exit(EXT_ERR_CONFIG_FILE);  // �迭�� �ƴϸ� ����
        }
    }
    else {
        fprintf(stderr, "Missing 'monitor_directories' in config file\n");
        config_destroy(&cfg);         // ���� ��ü ����
        exit(EXT_ERR_CONFIG_FILE);    // ���丮�� ������ ����
    }

    config_destroy(&cfg);             // ���� ��ü ����
}

// ���丮 ���� �߰� �Լ�
void add_watch_recursive(const char* path) {
    DIR* dir = opendir(path);         // ���丮 ����
    if (!dir) {
        perror("Error opening directory");
        return;                       // ����
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {  // ���丮 ��Ʈ�� �б�
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;                 // '.'�� '..'�� ����
        }

        // ���� ���丮
        char subPath[512];
        snprintf(subPath, sizeof(subPath), "%s/%s", path, entry->d_name);  // ���� ���丮 ��� ����

        struct stat pathStat;
        if (stat(subPath, &pathStat) == 0 && S_ISDIR(pathStat.st_mode)) {  // ���丮���� Ȯ��
            add_watch_recursive(subPath);  // ���� ���丮 ���� �߰�
        }
    }

    int wd = inotify_add_watch(IeventQueue, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE_SELF);
    if (wd == -1) {
        fprintf(stderr, "Error adding watch for %s\n", path);
    }
    else {
        // wd�� ���丮 ���� ����
        strncpy(watchDescriptors[watchDescriptorCount].path, path, 512);  // ���丮 ��� ����
        watchDescriptors[watchDescriptorCount].wd = wd;  // wd ����
        watchDescriptorCount++;       // wd ���� ����

        printf("Watching: %s\n", path);  // ���� ���丮 ���
    }

    closedir(dir);                    // ���丮 �ݱ�
}

// wd�� ��η� ��ȯ�ϴ� �Լ� (watchMask �ʵ�� inotify_event ����ü�� �������� �����Ƿ� ����)
const char* get_path_from_wd(int wd) {
    for (int i = 0; i < watchDescriptorCount; ++i) {
        if (watchDescriptors[i].wd == wd) {  // wd�� ��ġ�ϸ� ��� ��ȯ
            return watchDescriptors[i].path;
        }
    }
    return "Unknown path";  // ��θ� ã�� ���� ���
}

// ���� Ȯ���� ���͸� �Լ�
int has_filtered_extension(const char* filename) {
    if (strlen(filteredExtension) == 0) return 0;  // ���͸� Ȯ���ڰ� ���� ��� ���͸����� ����

    const char* ext = strrchr(filename, '.');  // ���ϸ��� Ȯ���ڸ� ����
    if (ext != NULL && strcmp(ext + 1, filteredExtension) == 0) {
        return 1;           // ���͸� Ȯ���ڿ� ��ġ�ϸ� 1 ��ȯ
    }
    return 0;               // Ȯ���ڰ� ��ġ���� ������ 0 ��ȯ
}

// ���͸� Ȯ���� Ȯ�� �Լ�
void check_filtered_extension() {
    if (strlen(filteredExtension) > 0) {
        printf("Filtering files with extension: .%s\n", filteredExtension);  // ���͸� Ȯ���� ���
    }
    else {
        printf("No extensions are filtered\n");  // ���͸� Ȯ���ڰ� ������ ���
    }
}

// �̺�Ʈ ó�� �Լ�
void process_event(const struct inotify_event* watchEvent) {
    if (watchEvent->len > 0) {
        const char* filename = watchEvent->name;

        // ���͸��� Ȯ������ ��� ó������ ����
        if (has_filtered_extension(filename)) {
            return;         // ���͸��� ������ �̺�Ʈ�� ó������ ����
        }

        char notificationMessage[1024];  // �̺�Ʈ �޽��� ����
        char fullPath[512]; // ������ ��ü ��� ����
        char eventTime[64]; // �̺�Ʈ �߻� �ð� ����
        time_t currentTime = time(NULL);  // ���� �ð� ���

        // �߻� �ð� ����
        strftime(eventTime, sizeof(eventTime), "%Y-%m-%d %H:%M:%S", localtime(&currentTime));

        const char* basePath = get_path_from_wd(watchEvent->wd);  // wd�� �ش��ϴ� ��� ���
        snprintf(fullPath, sizeof(fullPath), "%s/%s", basePath, watchEvent->name);  // ��ü ��� ����
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

        // ������ �̺�Ʈ�� 1�� �̻� ������ �ΰ� �߻��� ��� �α� ���
        if (difftime(currentTime, lastEventTime) >= 1) {
            lastEventTime = currentTime;  // ������ �̺�Ʈ �ð� ����
            log_event(notificationMessage);  // �α׿� �̺�Ʈ ���
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor CONFIG_PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    char monitoredDirs[512][512];  // ����͸��� ���丮 �迭
    int dirCount = 0;       // ����͸��� ���丮 ����

    read_config(argv[1], monitoredDirs, &dirCount);  // ���� ���� �б�
    init_log_file(logFilePath);  // �α� ���� �ʱ�ȭ

    check_filtered_extension();  // ���͸� Ȯ���� Ȯ��

    IeventQueue = inotify_init();
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    for (int i = 0; i < dirCount; ++i) {
        add_watch_recursive(monitoredDirs[i]);  // ���丮 ���� �߰�
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