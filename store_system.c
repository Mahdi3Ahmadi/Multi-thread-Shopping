// store_system.c

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>   // For stat()
#include <fcntl.h>      // For shm_open
#include <sys/mman.h>   // For mmap
#include <semaphore.h>
#include <ctype.h>      // For tolower()
#include <limits.h>     // For PATH_MAX
#include <time.h>       // For logging timestamps
#include "store_system.h"
// Function to compare strings case-insensitively
int strcasecmp_custom(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (tolower((unsigned char)*s1) != tolower((unsigned char)*s2))
            return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Function to trim newline characters
void trim_newline(char *str) {
    size_t len = strlen(str);
    if(len > 0 && str[len-1] == '\n') {
        str[len-1] = '\0';
    }
}

// Function to parse product details from a file
int parse_product_file(const char *filepath, Product *product) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        perror("Failed to open product file");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Name:", 5) == 0) {
            sscanf(line, "Name: %[^\n]", product->name);
            trim_newline(product->name);
        } else if (strncmp(line, "Price:", 6) == 0) {
            sscanf(line, "Price: %lf", &product->price);
        } else if (strncmp(line, "Score:", 6) == 0) {
            sscanf(line, "Score: %lf", &product->score);
        } else if (strncmp(line, "Entity:", 7) == 0) {
            sscanf(line, "Entity: %d", &product->entity);
        } else if (strncmp(line, "Last Modified:", 14) == 0) {
            int year, month, day, hour, minute, second;
            if (sscanf(line, "Last Modified: %d-%d-%d %d:%d:%d",
                       &year, &month, &day, &hour, &minute, &second) == 6) {
                product->last_modified.tm_year = year - 1900;
                product->last_modified.tm_mon = month - 1;
                product->last_modified.tm_mday = day;
                product->last_modified.tm_hour = hour;
                product->last_modified.tm_min = minute;
                product->last_modified.tm_sec = second;
            } else {
                fprintf(stderr, "Failed to parse Last Modified field\n");
                fclose(file);
                return 0;
            }
        }
    }

    fclose(file);
    return 1;
}

// Function to log activities with timestamps
void log_activity(const char *activity) {
    pthread_mutex_lock(&log_mutex);
    FILE *fp = fopen(temp_log_file, "a");
    if (fp == NULL) {
        perror("Failed to open log file");
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    // Get current time
    time_t now = time(NULL);
    char time_str[64];
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(fp, "[%s] %s\n", time_str, activity);
    fclose(fp);
    pthread_mutex_unlock(&log_mutex);
}

// Product-reading thread function
void *product_thread(void *arg) {
    ProductThreadData *data = (ProductThreadData *)arg;
    Product product;
    // Get PID and TID
    pid_t pid = getpid();
    pthread_t tid = pthread_self();

    printf("Product Thread PID: %d, TID: %lu\n", pid, tid);
    if (parse_product_file(data->filepath, &product)) {
        // Check if product is in shopping list
        for (int i = 0; i < shared_data->total_items; i++) {
            if (strcasecmp_custom(shared_data->shopping_list[i].name, product.name) == 0) {
                // Log the activity
                char activity[MAX_LOG_FILE];
                snprintf(activity, sizeof(activity), "Thread %lu (PID: %d) read product '%s' with Price: %.2lf and Score: %.2lf",
                         tid, pid, product.name, product.price, product.score);
                log_activity(activity);

                // Update shared data (e.g., store_total_scores)
                sem_wait(sem);
                shared_data->store_total_scores[data->store_id - 1] += product.price * product.score * shared_data->shopping_list[i].quantity;
                // Set the product's score in shopping list if not already set
                if (shared_data->shopping_list[i].score == 0.0) {
                    shared_data->shopping_list[i].score = product.score;
                }
                sem_post(sem);
            }
        }
    }

    pthread_exit(NULL);
}

// Valuation thread function
void *valuation_thread(void *arg) {
    // For the selected store, shared_data->total_score is already set
    printf("Valuation Thread: Total Score = %.2lf\n", shared_data->total_score);
    char activity[MAX_LOG_FILE];
    snprintf(activity, sizeof(activity), "Valuation Thread: Total Score = %.2lf", shared_data->total_score);
    log_activity(activity);
    pthread_exit(NULL);
}

// Finalization thread function
void *finalization_thread(void *arg) {
    if (purchase_ceiling_defined) {
        if (shared_data->total_score > purchase_ceiling) {
            printf("Finalization Thread: Total score exceeds the purchase ceiling of %.2lf. Purchase cannot be completed.\n", purchase_ceiling);
            log_activity("Finalization Thread: Purchase exceeded ceiling and was not completed.");
            pthread_exit(NULL);
        }
    }

    // Finalize purchase
    printf("Finalization Thread: Purchase finalized with total score %.2lf\n", shared_data->total_score);
    log_activity("Finalization Thread: Purchase finalized.");

    // Apply discount if applicable (e.g., repeat purchase)
    // This would require tracking previous purchases, which can be implemented as needed

    pthread_exit(NULL);
}

// Re-rating thread function
void *re_rating_thread(void *arg) {
    for (int i = 0; i < shared_data->total_items; i++) {
        printf("Rate the product '%s' (current average score: %.2lf): ", shared_data->shopping_list[i].name, shared_data->shopping_list[i].score);
        double user_rating;
        if (scanf("%lf", &user_rating) != 1) {
            printf("Invalid input. Skipping rating for '%s'.\n", shared_data->shopping_list[i].name);
            continue;
        }

        // Update the product's score (average)
        sem_wait(sem);
        shared_data->shopping_list[i].score = (shared_data->shopping_list[i].score + user_rating) / 2.0;
        sem_post(sem);
        
        // Log the rating
        char activity[MAX_LOG_FILE];
        snprintf(activity, sizeof(activity), "Re-Rating Thread: Updated score for '%s' to %.2lf",
                 shared_data->shopping_list[i].name, shared_data->shopping_list[i].score);
        log_activity(activity);
    }

    pthread_exit(NULL);
}

// Function to handle subcategory processes and create threads
void handle_subcategory_process(int store_id, char *subcategory_name) {
    char subcategory_path[PATH_MAX];
    snprintf(subcategory_path, sizeof(subcategory_path), "%s/Store%d/%s", BASE_DIR, store_id, subcategory_name);
    printf("Store %d Subcategory Path: %s\n", store_id, subcategory_path); // Debug print

    DIR *dir = opendir(subcategory_path);
    if (!dir) {
        perror("Failed to open subcategory directory");
        exit(EXIT_FAILURE);
    }

    struct dirent *entry;
    pthread_t product_threads[MAX_PRODUCTS];
    ProductThreadData thread_data[MAX_PRODUCTS];
    int thread_count = 0;

    while ((entry = readdir(dir)) != NULL && thread_count < MAX_PRODUCTS) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Construct full filepath
        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", subcategory_path, entry->d_name);

        // Initialize thread data
        strncpy(thread_data[thread_count].filepath, filepath, PATH_MAX);
        thread_data[thread_count].pid = getpid();
        thread_data[thread_count].tid = pthread_self();
        thread_data[thread_count].store_id = store_id;

        // Create thread
        if (pthread_create(&product_threads[thread_count], NULL, product_thread, &thread_data[thread_count]) != 0) {
            perror("Failed to create product thread");
            continue;
        }

        thread_count++;
    }

    // Join all product threads
    for (int i = 0; i < thread_count; i++) {
        pthread_join(product_threads[i], NULL);
    }

    closedir(dir);
    exit(EXIT_SUCCESS); // Subcategory process exits after completing
}

// Function to handle store processes
void handle_store_process(int store_id) {
    char store_path[PATH_MAX];
    snprintf(store_path, sizeof(store_path), "%s/Store%d", BASE_DIR, store_id);
    printf("Store %d Path: %s\n", store_id, store_path); // Debug print

    DIR *dir = opendir(store_path);
    if (!dir) {
        perror("Failed to open store directory");
        exit(EXIT_FAILURE);
    }

    struct dirent *entry;
    pid_t pid;
    int subcategory_count = 0;

    while ((entry = readdir(dir)) != NULL && subcategory_count < NUM_SUBCATEGORIES) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Construct subcategory path
        char subcategory_path[PATH_MAX];
        snprintf(subcategory_path, sizeof(subcategory_path), "%s/%s", store_path, entry->d_name);

        // Check if it's a directory
        struct stat path_stat;
        if (stat(subcategory_path, &path_stat) != 0) {
            perror("Failed to stat subcategory path");
            continue;
        }

        if (!S_ISDIR(path_stat.st_mode))
            continue;

        // Fork a child process for the subcategory
        pid = fork();
        if (pid < 0) {
            perror("Failed to fork subcategory process");
            continue;
        } else if (pid == 0) {
            // Child process handles the subcategory
            // Close the parent directory in the child process
            closedir(dir);
            // Handle subcategory
            handle_subcategory_process(store_id, entry->d_name);
            exit(EXIT_SUCCESS); // Ensure child exits after handling
        }

        subcategory_count++;
    }

    closedir(dir);

    // Wait for all subcategory processes
    for (int i = 0; i < subcategory_count; i++) {
        wait(NULL);
    }

    exit(EXIT_SUCCESS); // Store process exits after completing
}

// Function to create main process threads
void create_main_threads(pthread_t *val_thread, pthread_t *fin_thread, pthread_t *rate_thread) {
    // Create Valuation Thread
    if (pthread_create(val_thread, NULL, valuation_thread, NULL) != 0) {
        perror("Failed to create Valuation Thread");
    }

    // Create Finalization Thread
    if (pthread_create(fin_thread, NULL, finalization_thread, NULL) != 0) {
        perror("Failed to create Finalization Thread");
    }

    // Create Re-Rating Thread
    if (pthread_create(rate_thread, NULL, re_rating_thread, NULL) != 0) {
        perror("Failed to create Re-Rating Thread");
    }
}

int main() {
    // Step 1: User Input (Username)
    printf("Enter your name: ");
    scanf("%49s", username);
    printf("Welcome, %s! Parent Process PID: %d\n\n", username, getpid());

    // Step 2: Initialize Shared Memory
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to create shared memory");
        exit(EXIT_FAILURE);
    }

    // Set the size of shared memory
    if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
        perror("Failed to set size of shared memory");
        shm_unlink(SHM_NAME);
        exit(EXIT_FAILURE);
    }

    // Map the shared memory
    shared_data = mmap(0, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data == MAP_FAILED) {
        perror("Failed to map shared memory");
        shm_unlink(SHM_NAME);
        exit(EXIT_FAILURE);
    }

    // Initialize shared data
    shared_data->total_items = 0;
    for (int i = 0; i < NUM_STORES; i++) {
        shared_data->store_total_scores[i] = 0.0;
    }
    shared_data->total_score = 0.0;

    // Step 3: Initialize Semaphore
    sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("Failed to open semaphore");
        munmap(shared_data, sizeof(SharedData));
        shm_unlink(SHM_NAME);
        exit(EXIT_FAILURE);
    }

    // Step 4: Setup Logging
    snprintf(temp_log_file, sizeof(temp_log_file), "%s/%s_Order.log", log_directory, username);
    // Clear existing log file
    FILE *fp = fopen(temp_log_file, "w");
    if (fp != NULL) {
        fclose(fp);
    }

    // Step 5: User Input (Shopping List)
    printf("How many products do you want to order? ");
    scanf("%d", &shared_data->total_items);

    if (shared_data->total_items <= 0 || shared_data->total_items > MAX_PRODUCTS) {
        printf("Invalid number of products. Exiting...\n");
        // Cleanup
        munmap(shared_data, sizeof(SharedData));
        shm_unlink(SHM_NAME);
        sem_close(sem);
        sem_unlink(SEM_NAME);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < shared_data->total_items; i++) {
        printf("Enter name of product %d: ", i + 1);
        scanf(" %[^\n]s", shared_data->shopping_list[i].name);
        printf("Enter quantity for %s: ", shared_data->shopping_list[i].name);
        scanf("%d", &shared_data->shopping_list[i].quantity);
        // Initialize score to 0.0, will be updated if product is found
        shared_data->shopping_list[i].score = 0.0;
    }

    printf("Enter purchase ceiling (0 if not defined): ");
    scanf("%lf", &purchase_ceiling);
    if (purchase_ceiling > 0) {
        purchase_ceiling_defined = 1;
    }

    // Step 6: Create Store Child Processes
    pid_t store_pids[NUM_STORES];
    for (int i = 0; i < NUM_STORES; i++) {
        store_pids[i] = fork();
        if (store_pids[i] < 0) {
            perror("Failed to fork store process");
            // Cleanup before exiting
            munmap(shared_data, sizeof(SharedData));
            shm_unlink(SHM_NAME);
            sem_close(sem);
            sem_unlink(SEM_NAME);
            exit(EXIT_FAILURE);
        } else if (store_pids[i] == 0) {
            // Child process handles the store
            handle_store_process(i + 1);
            exit(EXIT_SUCCESS); // Ensure child exits after handling
        }
    }

    // Step 7: Wait for Store Processes to Complete
    for (int i = 0; i < NUM_STORES; i++) {
        waitpid(store_pids[i], NULL, 0);
    }

    // Step 8: Display Order Lists from All Stores
    printf("\n--- Order Lists from All Stores ---\n");
    for (int i = 0; i < NUM_STORES; i++) {
        printf("Store %d Total Score: %.2lf\n", i + 1, shared_data->store_total_scores[i]);
    }

    // Step 9: Ask User to Select Preferred Store
    int selected_store = 0;
    while (1) {
        printf("\nSelect a store to proceed with purchase (1-%d): ", NUM_STORES);
        if (scanf("%d", &selected_store) != 1 || selected_store < 1 || selected_store > NUM_STORES) {
            printf("Invalid selection. Please try again.\n");
            // Clear input buffer
            while (getchar() != '\n');
        } else {
            break;
        }
    }
    

    printf("You selected Store %d.\n", selected_store);
    char selection_activity[MAX_LOG_FILE];
    snprintf(selection_activity, sizeof(selection_activity), "User selected Store %d for purchase.", selected_store);
    log_activity(selection_activity);

    // Step 10: Set shared_data->total_score to the selected store's total score
    shared_data->total_score = shared_data->store_total_scores[selected_store - 1];

    // Step 11: Create Valuation, Finalization, and Re-Rating Threads for Selected Store
    pthread_t val_thread, fin_thread, rate_thread;

    // Create Valuation Thread
    if (pthread_create(&val_thread, NULL, valuation_thread, NULL) != 0) {
        perror("Failed to create Valuation Thread");
    }

    // Create Finalization Thread
    if (pthread_create(&fin_thread, NULL, finalization_thread, NULL) != 0) {
        perror("Failed to create Finalization Thread");
    }

    // Create Re-Rating Thread
    if (pthread_create(&rate_thread, NULL, re_rating_thread, NULL) != 0) {
        perror("Failed to create Re-Rating Thread");
    }

    // Step 12: Wait for Main Threads
    pthread_join(val_thread, NULL);
    pthread_join(fin_thread, NULL);
    pthread_join(rate_thread, NULL);

    
    // Later, after store selection
    char final_log_file[PATH_MAX];
    snprintf(final_log_file, sizeof(final_log_file), "./dataset/store%d/%s_Order.log", selected_store, username);
    
    // Move temporary log to the final location
    if (rename(temp_log_file, final_log_file) != 0) {
        perror("Failed to move temporary log file");
        // در صورت نیاز می‌توانید اینجا برنامه را متوقف کنید یا پیام خطا نشان دهید
        } else {
            // delete temp_log_file
            if (remove(temp_log_file) != 0) {
                perror("Failed to delete temporary log file");
            } else {
                printf("Temporary log file deleted successfully.\n");
            }
    }


    // Step 13: Cleanup
    munmap(shared_data, sizeof(SharedData));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    sem_close(sem);
    sem_unlink(SEM_NAME);
    pthread_mutex_destroy(&log_mutex);

    printf("All processes and threads have completed. Parent Process (PID: %d) exiting.\n", getpid());

    return 0;
}