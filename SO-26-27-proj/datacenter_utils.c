#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <math.h>
#include <errno.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

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

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
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

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
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

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

void vm_destroy(DataCenter *dc, VM *vm) {
	if (dc == NULL || vm == NULL) return;

	Server *server = vm->server;
	if (server != NULL) {
		resources_add(&server->available, vm->type->required);

		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				memmove(&server->hosted_vms[j],
						&server->hosted_vms[j + 1],
						(server->num_hosted_vms - j - 1) * sizeof(VM *));
				server->num_hosted_vms--;
				server->hosted_vms[server->num_hosted_vms] = NULL;
				break;
			}
		}
	}

	Reservation *res = NULL;
	for (size_t i = 0; i < dc->num_reservations; i++) {
		for (size_t j = 0; j < dc->reservations[i].num_vms; j++) {
			if (dc->reservations[i].vms[j] == vm) {
				res = &dc->reservations[i];
				memmove(&res->vms[j],
						&res->vms[j + 1],
						(res->num_vms - j - 1) * sizeof(VM *));
				res->num_vms--;
				res->vms[res->num_vms] = NULL;
				break;
			}
		}
		if (res != NULL) break;
	}

	free(vm);

	if (res != NULL && res->num_vms == 0) {
		res->state = RES_STATE_FINISHED;
		size_t idx;
		for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == res)
				break;
		}
		if (idx < dc->num_reservations) {
			memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));
			dc->num_reservations--;
		}
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
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}