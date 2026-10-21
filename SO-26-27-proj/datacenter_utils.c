#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <math.h>
#include <errno.h>

VMType *VMType_exists_locked(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	pthread_rwlock_rdlock(&dc->vm_types_lock);
	VMType *found = VMType_exists_locked(dc, type_id);
	pthread_rwlock_unlock(&dc->vm_types_lock);

	return found;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * Must be called with the reservations lock held.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i]->id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_check_items(DataCenter *dc, Reservation *reservation) {
	size_t vms_count = 0;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		vms_count += item->amount;
		if(vms_count > MAX_RESERVATION_VMS){
			fprintf(stderr, "Reached max of VMS for a reservation.\n");
			return 1;
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

int reservation_register(DataCenter *dc, Reservation *reservation) {
	int ret = 0;

	pthread_mutex_lock(&dc->reservations_lock);
	if (reservation_exists(dc, reservation->id)) {
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		ret = 1;
	} else if (dc->num_reservations >= MAX_RESERVATIONS) {
		fprintf(stderr, "Max number of reservations reached\n");
		ret = 1;
	} else {
		dc->reservations[dc->num_reservations++] = reservation;
	}
	pthread_mutex_unlock(&dc->reservations_lock);

	return ret;
}

/**
 * Removes the reservation from the Data Center reservation list.
 * Must be called with the reservations lock held.
 *
 * @param dc Pointer to Data Center.
 * @param reservation Reservation to remove.
 */
static void reservation_remove(DataCenter *dc, Reservation *reservation) {
	for (size_t idx = 0; idx < dc->num_reservations; idx++) {
		if (dc->reservations[idx] == reservation) {
			memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation *));
			dc->num_reservations--;
			dc->reservations[dc->num_reservations] = NULL;
			return;
		}
	}
}

void reservation_unregister(DataCenter *dc, Reservation *reservation) {
	pthread_mutex_lock(&dc->reservations_lock);
	reservation_release(dc, reservation);
	reservation_remove(dc, reservation);
	pthread_mutex_unlock(&dc->reservations_lock);
}

/**
 * Marks which servers are used by the reservation.
 *
 * @param dc Pointer to Data Center.
 * @param reservation Reservation being checked.
 * @param used Array with num_servers entries, set to 1 for each used server.
 */
static void reservation_used_servers(DataCenter *dc, Reservation *reservation, char *used) {
	memset(used, 0, dc->num_servers);
	for (size_t i = 0; i < reservation->items_count; i++) {
		used[reservation->items[i].server_id - 1] = 1;
	}
}

void reservation_lock_servers(DataCenter *dc, Reservation *reservation) {
	char used[dc->num_servers];
	reservation_used_servers(dc, reservation, used);

	// Ascending order so that concurrent reservations never deadlock.
	for (size_t i = 0; i < dc->num_servers; i++) {
		if (used[i]) pthread_mutex_lock(&dc->servers[i].lock);
	}
}

void reservation_unlock_servers(DataCenter *dc, Reservation *reservation) {
	char used[dc->num_servers];
	reservation_used_servers(dc, reservation, used);

	for (size_t i = dc->num_servers; i > 0; i--) {
		if (used[i - 1]) pthread_mutex_unlock(&dc->servers[i - 1].lock);
	}
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];

	/* Copy the resources of the (locked) servers used so we can simulate the allocation. */
	for (size_t i = 0; i < reservation->items_count; i++) {
		size_t s = reservation->items[i].server_id - 1;
		available[s] = dc->servers[s].available;
		hosted_count[s] = dc->servers[s].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		size_t s = item->server_id - 1;

		for (size_t j = 0; j < item->amount; j++) {
			if (hosted_count[s] >= MAX_HOSTED_VMS) {
				fprintf(stderr,
						"Server \"%zu\" reached the maximum number of hosted VMs.\n",
						item->server_id);
				return 1;
			}

			if (!resources_can_fit(available[s], item->vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[s], item->vm_type->required);
			hosted_count[s]++; // One VM "reserved"
		}
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;
			snprintf(new_vm->reservation_id, sizeof(new_vm->reservation_id), "%s", reservation->id);

			double local_cpus = (dc->local_cpus > 0.0) ? dc->local_cpus : (dc->servers[0].total.cpu * (double)dc->num_servers);
			double total_dc_cpu = dc->servers[0].total.cpu * (double)dc->num_servers;
			double cpulimit_calc = 0.0;
			if (total_dc_cpu > 0.0) {
				cpulimit_calc = (100.0 * item->vm_type->required.cpu * local_cpus) / total_dc_cpu;
			}
			double max_cpulimit = 100.0 * local_cpus;
			if (cpulimit_calc > max_cpulimit) {
				cpulimit_calc = max_cpulimit;
			}
			int limit = (int)round(cpulimit_calc);
			if (limit < 1) {
				limit = 1;
			}
			new_vm->cpulimit = limit;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i]->id, reservation_id) == 0) {
			res = dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

/**
 * Returns the VM resources to its hosting server and removes the VM from the
 * server's hosted VM list. Takes the server lock.
 *
 * @param vm VM being removed from its server.
 */
static void vm_unhost(VM *vm) {
	Server *server = vm->server;
	if (server == NULL) return;

	pthread_mutex_lock(&server->lock);
	resources_add(&server->available, vm->type->required);

	for (size_t j = 0; j < server->num_hosted_vms; j++) {
		if (server->hosted_vms[j] == vm) {
			// Shift the remaining VMs one position to the left.
			memmove(&server->hosted_vms[j],
					&server->hosted_vms[j + 1],
					(server->num_hosted_vms - j - 1) * sizeof(VM *));
			server->num_hosted_vms--;
			server->hosted_vms[server->num_hosted_vms] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&server->lock);
}

void reservation_release(DataCenter *dc, Reservation *reservation) {
	(void)dc;

	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		vm_unhost(reservation->vms[i]);
		free(reservation->vms[i]);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;
}

void vm_destroy(DataCenter *dc, VM *vm) {
	if (dc == NULL || vm == NULL) return;

	vm_unhost(vm);

	Reservation *res = NULL;
	for (size_t i = 0; i < dc->num_reservations && res == NULL; i++) {
		Reservation *r = dc->reservations[i];
		if (r->state != RES_STATE_RUNNING) continue;
		for (size_t j = 0; j < r->num_vms; j++) {
			if (r->vms[j] == vm) {
				res = r;
				memmove(&res->vms[j],
						&res->vms[j + 1],
						(res->num_vms - j - 1) * sizeof(VM *));
				res->num_vms--;
				res->vms[res->num_vms] = NULL;
				break;
			}
		}
	}

	free(vm);

	if (res != NULL && res->num_vms == 0) {
		res->state = RES_STATE_FINISHED;
		reservation_remove(dc, res);
		free(res);
	}
}

void spawn_vm_child(VM *vm) {
	char vm_dir[MAX_PATH_SIZE];
	if (vm->reservation_id[0] != '\0') {
		snprintf(vm_dir, sizeof(vm_dir), "/tmp/CloudIST/%s/%s", vm->reservation_id, vm->id);
	} else {
		char res_id[MAX_STRING_SIZE] = {0};
		const char *last_underscore = strrchr(vm->id, '_');
		if (last_underscore != NULL) {
			size_t prefix_len = (size_t)(last_underscore - vm->id);
			if (prefix_len < sizeof(res_id)) {
				strncpy(res_id, vm->id, prefix_len);
				res_id[prefix_len] = '\0';
			}
		}
		snprintf(vm_dir, sizeof(vm_dir), "/tmp/CloudIST/%s/%s", res_id, vm->id);
	}
	if (chdir(vm_dir) != 0) {
		// Directory might not exist or failed to change; proceed anyway
	}

	// RAM limit
	struct rlimit lim_as;
	rlim_t ram_bytes = (rlim_t)vm->type->required.ram * (1024ULL * 1024ULL * 1024ULL);
	lim_as.rlim_cur = ram_bytes;
	lim_as.rlim_max = ram_bytes;
	setrlimit(RLIMIT_AS, &lim_as);

	// Disk file size limit
	struct rlimit lim_fsize;
	rlim_t disk_bytes = (rlim_t)vm->type->required.disk * (1024ULL * 1024ULL * 1024ULL);
	lim_fsize.rlim_cur = disk_bytes;
	lim_fsize.rlim_max = disk_bytes;
	setrlimit(RLIMIT_FSIZE, &lim_fsize);

	// CPU limit using cpulimit
	char cpulimit_str[32];
	snprintf(cpulimit_str, sizeof(cpulimit_str), "%d", vm->cpulimit > 0 ? vm->cpulimit : 100);

	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpulimit_str, "--", vm->type->exec_path, (char *)NULL);

	// Fallback if cpulimit is not installed
	if (errno == ENOENT) {
		execl(vm->type->exec_path, vm->type->exec_path, (char *)NULL);
	}

	perror("Failed to execute VM application");
	_exit(1);
}

int spawn_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];

		fflush(stdout);
		fflush(stderr);

		pid_t pid = fork();
		if (pid < 0) {
			perror("Failed to fork VM process");
			return 1;
		}

		if (pid == 0) {
			spawn_vm_child(vm);
			_exit(1);
		}

		vm->pid = pid;
		vm->state = VM_STATE_RUNNING;
	}

	return 0;
}

void wait_for_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];
		if (vm != NULL && vm->state == VM_STATE_RUNNING && vm->pid > 0) {
			int status;
			waitpid(vm->pid, &status, 0);
			vm->state = VM_STATE_TERMINATED;
		}
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_RESERVING: return "RESERVING";
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}