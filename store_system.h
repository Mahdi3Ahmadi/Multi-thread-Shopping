
#define BASE_DIR "./Dataset" // Adjust this path as per your system
#define NUM_STORES 3
#define NUM_SUBCATEGORIES 8
#define MAX_PRODUCTS 100
#define MAX_NAME_LEN 50
#define MAX_LOG_FILE 256
#define SHM_NAME "/store_shared_memory"

// Semaphore name
#define SEM_NAME "/store_semaphore"

// Structure to hold product details
typedef struct {
    char name[MAX_NAME_LEN];
    double price;
    double score;
    int entity;
    struct tm last_modified;
} Product;

// Structure to hold shopping list items
typedef struct {
    char name[MAX_NAME_LEN];
    int quantity;
    double score; // Average score for the product
} ShoppingItem;

// Structure for shared memory
typedef struct {
    ShoppingItem shopping_list[MAX_PRODUCTS];
    int total_items;
    double store_total_scores[NUM_STORES];
    double total_score; // For selected store
} SharedData;

// Structure for product-reading thread data
typedef struct {
    char filepath[PATH_MAX];
    pid_t pid;
    pthread_t tid;
    int store_id;
} ProductThreadData;

// Global variables for shared memory and semaphore
SharedData *shared_data;
sem_t *sem;

// Mutex for logging to prevent race conditions
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// User information
char username[MAX_NAME_LEN];
int purchase_ceiling_defined = 0;
double purchase_ceiling = 0.0;

// Log file path
char temp_log_file[MAX_LOG_FILE];
const char *log_directory = "./dataset";