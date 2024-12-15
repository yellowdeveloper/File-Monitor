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
#include <omp.h>

#define EXT_SUCCESS 0
#define EXT_ERR_TOO_FEW_ARGS 1
#define EXT_ERR_INIT_INOTIFY 2
#define EXT_ERR_ADD_WATCH 3
#define EXT_ERR_READ_INOTIFY 5
#define EXT_ERR_CONFIG_FILE 6

int IeventStatus = -1;
int IeventQueue = -1;
char* ProgramTitle = "file_monitor";
FILE* logFile = NULL;
GtkTextBuffer *log_buffer = NULL;
GtkWidget* file_list = NULL;
char** monitoredDirs = NULL;
int monitoredDirCount = 0;
char logFilePath[512] = "";

volatile bool keep_running = true;
volatile bool logBufferDirty = false;

void init_log_file(const char* path) {
    logFile = fopen(path, "a");
    if (!logFile) {
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    printf("Log file initialized at: %s\n", path);
}

gboolean update_log_buffer(gpointer data) {
    const char* eventMessage = (const char*)data;

    if (log_buffer) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(log_buffer, &end);
        gtk_text_buffer_insert(log_buffer, &end, eventMessage, -1);
        gtk_text_buffer_insert(log_buffer, &end, "\n", -1);
    }

    g_free(data);
    return FALSE;
}

void log_event(const char* eventMessage) {
    static char logBuffer[8192] = "";
    static size_t logBufferSize = 0;

    size_t messageLength = strlen(eventMessage) + 1; // Include newline
    if (logBufferSize + messageLength > sizeof(logBuffer) - 1) {
        if (logFile) {
            fprintf(logFile, "%s", logBuffer);
            fflush(logFile);
        }
        logBufferSize = 0;
        logBuffer[0] = '\0';
    }

    strcat(logBuffer, eventMessage);
    strcat(logBuffer, "\n");
    logBufferSize += messageLength;

    logBufferDirty = true;
    g_idle_add(update_log_buffer, g_strdup(eventMessage));
}

void flush_log_buffer() {
    static char logBuffer[8192] = "";
    if (logFile && logBufferDirty) {
        fprintf(logFile, "%s", logBuffer);
        fflush(logFile);
        logBuffer[0] = '\0';
        logBufferDirty = false;
    }
}

void read_config(const char* configPath) {
    config_t cfg;
    config_init(&cfg);

    if (!config_read_file(&cfg, configPath)) {
        fprintf(stderr, "Error reading config file %s: %s\n", configPath, config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EXT_ERR_CONFIG_FILE);
    }

    const char* logPath = NULL;
    if (config_lookup_string(&cfg, "log_file", &logPath)) {
        strncpy(logFilePath, logPath, sizeof(logFilePath));
    } else {
        fprintf(stderr, "Missing 'log_file' in config file\n");
        config_destroy(&cfg);
        exit(EXT_ERR_CONFIG_FILE);
    }

    config_setting_t* directories = config_lookup(&cfg, "monitor_directories");
    if (directories && config_setting_is_array(directories)) {
        monitoredDirCount = config_setting_length(directories);
        monitoredDirs = malloc(monitoredDirCount * sizeof(char*));
        for (int i = 0; i < monitoredDirCount; ++i) {
            const char* dir = config_setting_get_string_elem(directories, i);
            monitoredDirs[i] = strdup(dir);
        }
    } else {
        fprintf(stderr, "Missing or invalid 'monitor_directories' in config file\n");
        config_destroy(&cfg);
        exit(EXT_ERR_CONFIG_FILE);
    }

    config_destroy(&cfg);
}

void update_file_list(const char* path) {
    GtkListStore* store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(file_list)));
    gtk_list_store_clear(store);

    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, entry->d_name, -1);
            }
        }
        closedir(dir);
    }
}

void on_row_activated(GtkTreeView* tree_view, GtkTreePath* path, GtkTreeViewColumn* column, gpointer user_data) {
    GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gchar* selected_dir = NULL;
        gtk_tree_model_get(model, &iter, 0, &selected_dir, -1);

        if (selected_dir) {
            char new_path[512];
            snprintf(new_path, sizeof(new_path), "%s/%s", (char*)user_data, selected_dir);

            struct stat path_stat;
            if (stat(new_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
                update_file_list(new_path);
            }

            g_free(selected_dir);
        }
    }
}

void add_watch_recursive(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        perror("Error opening directory");
        return;
    }

    struct dirent* entry;
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

    int wd = inotify_add_watch(IeventQueue, path, IN_CREATE | IN_DELETE | IN_MODIFY);
    if (wd == -1) {
        fprintf(stderr, "Error adding watch for %s\n", path);
    } else {
        printf("Watching: %s\n", path);
    }

    closedir(dir);
}

GtkWidget* create_window(const char* root_path) {
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "File Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget* scrolled_log = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget* textView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textView), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled_log), textView);
    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));

    GtkWidget* scrolled_files = gtk_scrolled_window_new(NULL, NULL);
    file_list = gtk_tree_view_new();
    GtkListStore* store = gtk_list_store_new(1, G_TYPE_STRING);
    gtk_tree_view_set_model(GTK_TREE_VIEW(file_list), GTK_TREE_MODEL(store));
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes("Files", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_list), column);
    gtk_container_add(GTK_CONTAINER(scrolled_files), file_list);

    g_signal_connect(file_list, "row-activated", G_CALLBACK(on_row_activated), (gpointer)root_path);

    gtk_paned_pack1(GTK_PANED(paned), scrolled_log, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), scrolled_files, TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(window), paned);

    return window;
}

void signal_handler(int signum) {
    keep_running = false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "USAGE: file_monitor CONFIG_PATH\n");
        exit(EXT_ERR_TOO_FEW_ARGS);
    }

    read_config(argv[1]);
    init_log_file(logFilePath);

    IeventQueue = inotify_init();
    if (IeventQueue == -1) {
        fprintf(stderr, "Error initializing inotify instance\n");
        exit(EXT_ERR_INIT_INOTIFY);
    }

    #pragma omp parallel for
    for (int i = 0; i < monitoredDirCount; ++i) {
        add_watch_recursive(monitoredDirs[i]);
    }

    gtk_init(&argc, &argv);

    GtkWidget* window = create_window(monitoredDirs[0]);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    signal(SIGINT, signal_handler);

    pthread_t thread;
    if (pthread_create(&thread, NULL, inotify_thread, NULL) != 0) {
        perror("Error creating inotify thread");
        return EXIT_FAILURE;
    }

    gtk_widget_show_all(window);
    gtk_main();

    keep_running = false;
    pthread_join(thread, NULL);

    flush_log_buffer();

    for (int i = 0; i < monitoredDirCount; ++i) {
        free(monitoredDirs[i]);
    }
    free(monitoredDirs);

    if (logFile) fclose(logFile);
    return EXT_SUCCESS;
}
