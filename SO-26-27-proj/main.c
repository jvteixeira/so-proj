#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"

/**
 * Signal handler for SIGCHLD to ensure interrupted system calls or sleeps
 * trigger early wakeups for process reaping.
 *
 * @param sig Signal number (SIGCHLD).
 */
static void handle_sigchld(int sig) {
	(void)sig;
}

/**
 * Shared state between the worker threads: the list of .conf files and the
 * index of the next file to be processed.
 */
typedef struct {
	DataCenter *dc;
	int dir_fd;
	const char **files;
	int num_files;
	int next_file;          // Protected by lock.
	pthread_mutex_t lock;
} WorkQueue;

void aux_executer(DataCenter *dc, int fd);

/**
 * Worker thread: repeatedly takes the next .conf file (in alphabetical order)
 * and executes its commands, until no files are left.
 *
 * @param arg Pointer to the shared WorkQueue.
 */
static void *worker_thread(void *arg) {
	WorkQueue *queue = (WorkQueue *)arg;

	while (1) {
		pthread_mutex_lock(&queue->lock);
		int i = queue->next_file < queue->num_files ? queue->next_file++ : -1;
		pthread_mutex_unlock(&queue->lock);

		if (i < 0) break;

		int fd = openat(queue->dir_fd, queue->files[i], O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "Failed to open %s\n", queue->files[i]);
			continue;
		}
		aux_executer(queue->dc, fd);
		close(fd);
	}

	return NULL;
}

/**
 * Processes every file in the list using at most max_threads worker threads.
 *
 * @param dc Pointer to Data Center.
 * @param dir_fd File descriptor of the input directory.
 * @param files Names of the .conf files, in alphabetical order.
 * @param num_files Number of files.
 * @param max_threads Maximum number of worker threads.
 */
static void process_files(DataCenter *dc, int dir_fd, const char **files,
		int num_files, size_t max_threads) {
	WorkQueue queue = {
		.dc = dc,
		.dir_fd = dir_fd,
		.files = files,
		.num_files = num_files,
		.next_file = 0
	};
	pthread_mutex_init(&queue.lock, NULL);

	// There is no point in having more threads than files.
	size_t num_threads = max_threads < (size_t)num_files ? max_threads : (size_t)num_files;
	pthread_t threads[num_threads];
	size_t created = 0;

	for (; created < num_threads; created++) {
		if (pthread_create(&threads[created], NULL, worker_thread, &queue) != 0) {
			fprintf(stderr, "Failed to create worker thread.\n");
			break;
		}
	}

	// If no thread could be created, the main thread does the work.
	if (created == 0) {
		worker_thread(&queue);
	}

	for (size_t i = 0; i < created; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_mutex_destroy(&queue.lock);
}

/**
 * Command execution loop for reading and dispatching CloudIST commands from
 * a file descriptor.
 *
 * @param dc Pointer to Data Center.
 * @param fd File descriptor to read commands from.
 */
void aux_executer(DataCenter *dc, int fd) {
	while(1){
		datacenter_reap_vms(dc);
		switch (get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				if(datacenter_define_VM(dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}

				printf("VM successfully defined!\n");

				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0};

				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}

				printf("Reservation made successfully!\n");

				break;
			}

			case CMD_EXECUTE: {
				char id[MAX_STRING_SIZE];
			
				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				printf("Finished reservation execution!\n");
				break;
			}
			case CMD_LIST:
				if (datacenter_list(dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}

				break;
			case CMD_WAIT: {

				unsigned int delay;

				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait_dc(dc, delay);
				break;
			}
			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				datacenter_reap_vms(dc);
				return;
		}
	}
}

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	struct sigaction sa;
	sa.sa_handler = handle_sigchld;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	if (argc < 6 || argc > 8) {
		fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <local_cores> [<input_dir> [<max_threads>]]\n",
			argv[0]);
		return 1;
  	}

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0){
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	Resources resources = {
    	.ram = ram,
    	.disk = disk,
    	.cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	double local_cores;
	const char *input_dir = (argc >= 7) ? argv[6] : NULL;
	size_t max_threads = 1;

	if (parse_double_arg(argv[5], &local_cores) != 0 || local_cores <= 0.0) {
		fprintf(stderr, "Invalid local cores argument: %s\n", argv[5]);
		datacenter_destroy(&dc);
		return 1;
	}

	if (argc == 8 && (parse_size_t_arg(argv[7], &max_threads) != 0 || max_threads == 0)) {
		fprintf(stderr, "Invalid max threads argument: %s\n", argv[7]);
		datacenter_destroy(&dc);
		return 1;
	}

	datacenter_set_local_cpus(&dc, local_cores);

	if (input_dir != NULL) {
		int dir_fd = validate_directory_arg(input_dir);
		if (dir_fd < 0) {
			fprintf(stderr, "Invalid input directory: %s\n", input_dir);
			datacenter_destroy(&dc);
			return 1;
		}

		const char **files = NULL;
		int c = parse_directory_arg(input_dir, &files);
		if (c > 0 && files != NULL) {
			process_files(&dc, dir_fd, files, c, max_threads);
			for (int i = 0; i < c; i++) {
				free((void *)files[i]);
			}
			free(files);
		}
		close(dir_fd);
	} else {
		aux_executer(&dc, STDIN_FILENO);
	}

	while (datacenter_has_running_vms(&dc)) {
		datacenter_wait_dc(&dc, 50);
	}

	datacenter_destroy(&dc);
	return 0;
}