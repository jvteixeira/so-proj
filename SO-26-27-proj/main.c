#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

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

	if (argc < 5 || argc > 7) {
		fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> [<local_cores>] [<input_dir>]\n", 
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

	double local_cores = 0.0;
	const char *input_dir = NULL;

	if (argc == 7) {
		if (parse_double_arg(argv[5], &local_cores) != 0 || local_cores <= 0.0) {
			fprintf(stderr, "Invalid local cores argument.\n");
			datacenter_destroy(&dc);
			return 1;
		}
		input_dir = argv[6];
	} else if (argc == 6) {
		if (path_exists(argv[5])) {
			input_dir = argv[5];
			local_cores = cpu * (double)servers;
		} else {
			if (parse_double_arg(argv[5], &local_cores) != 0 || local_cores <= 0.0) {
				fprintf(stderr, "Invalid argument: %s\n", argv[5]);
				datacenter_destroy(&dc);
				return 1;
			}
			input_dir = NULL;
		}
	} else { // argc == 5
		local_cores = cpu * (double)servers;
		input_dir = NULL;
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
			for (int i = 0; i < c; i++) {
				int fd = openat(dir_fd, files[i], O_RDONLY);
				if (fd >= 0) {
					aux_executer(&dc, fd);
					close(fd);
				}
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