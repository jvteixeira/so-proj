#include "datacenter.h"
#include "datacenter_utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "parser.h"
#include <sys/wait.h>

#define BUFFER_SIZE 4096

static DataCenter *g_current_dc = NULL;

void datacenter_set_local_cpus(DataCenter *dc, double local_cpus) {
  if (dc != NULL) {
    dc->local_cpus = local_cpus;
  }
}

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;
  pthread_rwlock_init(&dc->vm_types_lock, NULL);

  dc->num_reservations = 0;
  pthread_mutex_init(&dc->reservations_lock, NULL);

  dc->configured = 0;
  dc->local_cpus = 0.0;
  g_current_dc = dc;
}

void datacenter_destroy(DataCenter *dc) {
  if (dc == NULL) return;

  datacenter_reap_vms(dc);
  while (datacenter_has_running_vms(dc)) {
    datacenter_wait_dc(dc, 20);
  }

  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
    pthread_mutex_destroy(&dc->servers[i].lock);
  }
  for (size_t i = 0; i < dc->num_reservations; i++) {
    free(dc->reservations[i]);
  }
  dc->num_reservations = 0;
  pthread_mutex_destroy(&dc->reservations_lock);
  pthread_rwlock_destroy(&dc->vm_types_lock);
  free(dc->servers);
  dc->servers = NULL;
  dc->num_servers = 0;
  dc->configured = 0;
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    pthread_mutex_init(&dc->servers[i].lock, NULL);
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  VMType resolved_type = *type;
  char abs_exec[MAX_PATH_SIZE];
  if (absolute_path(type->exec_path, abs_exec, sizeof(abs_exec)) == 0) {
    strncpy(resolved_type.exec_path, abs_exec, sizeof(resolved_type.exec_path) - 1);
    resolved_type.exec_path[sizeof(resolved_type.exec_path) - 1] = '\0';
  }
  char abs_input[MAX_PATH_SIZE];
  if (absolute_path(type->input_folder, abs_input, sizeof(abs_input)) == 0) {
    strncpy(resolved_type.input_folder, abs_input, sizeof(resolved_type.input_folder) - 1);
    resolved_type.input_folder[sizeof(resolved_type.input_folder) - 1] = '\0';
  }

  // Check and insert atomically so two threads cannot define the same type.
  int ret = 0;
  pthread_rwlock_wrlock(&dc->vm_types_lock);
  if (VMType_exists_locked(dc, type->id)) {
    fprintf(stderr, "VM type with id %s already exists.\n", type->id);
    ret = 1;
  } else if (dc->num_vm_types >= MAX_VM_TYPES) {
    fprintf(stderr, "Maximum of VM types reached.\n");
    ret = 1;
  } else {
    dc->vm_types[dc->num_vm_types++] = resolved_type;
  }
  pthread_rwlock_unlock(&dc->vm_types_lock);

  return ret;
}

/* Função auxiliar para criar diretoria ignorando erro se já existir */
static int ensure_dir_exists(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

/* Copia um único ficheiro normal usando descritores POSIX */
static int copy_file(const char *src_path, const char *dst_path, mode_t mode) {
    int src_fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) return -1;

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (dst_fd < 0) {
      close(src_fd);
      return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
      char *out_ptr = buffer;
      ssize_t bytes_to_write = bytes_read;
      while (bytes_to_write > 0) {
        ssize_t bytes_written = write(dst_fd, out_ptr, (size_t)bytes_to_write);
        if (bytes_written < 0) {
          close(src_fd);
          close(dst_fd);
          return -1;
        }
        bytes_to_write -= bytes_written;
        out_ptr += bytes_written;
      }
    }

    close(src_fd);
    close(dst_fd);
    return (bytes_read < 0) ? -1 : 0;
}

/* Copia recursivamente todo o conteúdo de src_path para dst_path */
static int copy_directory_recursive(const char *src_path, const char *dst_path) {
  struct stat st;
  if (stat(src_path, &st) != 0) return -1;

  if (ensure_dir_exists(dst_path, st.st_mode) != 0) {
    return -1;
  }

  DIR *dir = opendir(src_path);
  if (!dir) return -1;

  struct dirent *entry;
  char src_child[PATH_MAX];
  char dst_child[PATH_MAX];

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    snprintf(src_child, sizeof(src_child), "%s/%s", src_path, entry->d_name);
    snprintf(dst_child, sizeof(dst_child), "%s/%s", dst_path, entry->d_name);

    struct stat child_st;
    if (lstat(src_child, &child_st) != 0) {
      closedir(dir);
      return -1;
    }

    if (S_ISDIR(child_st.st_mode)) {
      if (copy_directory_recursive(src_child, dst_child) != 0) {
        closedir(dir);
        return -1;
      }
    } else if (S_ISREG(child_st.st_mode)) {
      if (copy_file(src_child, dst_child, child_st.st_mode) != 0) {
        closedir(dir);
        return -1;
      }
    }
  }
  closedir(dir);
  return 0;
}

/**
 * Creates /tmp/CloudIST/<ID-reserva>/<ID-VM> for every VM of the reservation
 * and copies the input folder of each VM type into it.
 *
 * @param reservation Reservation whose folders are prepared.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
static int prepare_reservation_dirs(Reservation *reservation) {
  if (ensure_dir_exists("/tmp/CloudIST", 0755) != 0) {
    perror("Erro ao criar /tmp/CloudIST");
    return 1;
  }

  // Cria a pasta da reserva: /tmp/CloudIST/<ID-reserva>
  char res_path[MAX_PATH_SIZE];
  snprintf(res_path, sizeof(res_path), "/tmp/CloudIST/%s", reservation->id);
  if (ensure_dir_exists(res_path, 0755) != 0) {
    perror("Erro ao criar pasta da reserva");
    return 1;
  }

  // Cria a pasta de cada VM e copia os ficheiros de entrada
  for (size_t i = 0; i < reservation->num_vms; i++) {
    char vm_path[MAX_PATH_SIZE];
    snprintf(vm_path, sizeof(vm_path), "/tmp/CloudIST/%s/%s", reservation->id, reservation->vms[i]->id);

    const char *src_folder = reservation->vms[i]->type->input_folder;

    if (copy_directory_recursive(src_folder, vm_path) != 0) {
      fprintf(stderr, "Erro ao copiar inputs de %s para %s\n", src_folder, vm_path);
      return 1;
    }
  }

  return 0;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if (reservation_check_items(dc, reservation) != 0) {
    return 1;
  }

  // Heap allocated so its address stays valid while other threads change the list.
  Reservation *res = malloc(sizeof(Reservation));
  if (res == NULL) {
    fprintf(stderr, "Failed to allocate reservation.\n");
    return 1;
  }
  *res = *reservation;
  res->num_vms = 0;
  res->state = RES_STATE_RESERVING;

  // Claims the id. While RESERVING the reservation cannot be executed.
  if (reservation_register(dc, res) != 0) {
    free(res);
    return 1;
  }

  // Only the servers used by this reservation are locked, so reservations on
  // different servers are validated and committed in parallel.
  reservation_lock_servers(dc, res);
  int failed = reservation_validate(dc, res) != 0 || reservation_commit(dc, res) != 0;
  reservation_unlock_servers(dc, res);

  if (failed) {
    reservation_unregister(dc, res);
    free(res);
    return 1;
  }

  // Copying the input folders is the slowest step, so it is done without locks.
  if (prepare_reservation_dirs(res) != 0) {
    reservation_unregister(dc, res);
    free(res);
    return 1;
  }

  pthread_mutex_lock(&dc->reservations_lock);
  res->state = RES_STATE_PENDING;
  pthread_mutex_unlock(&dc->reservations_lock);

  return 0;
}


int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  datacenter_reap_vms(dc);

  // The lock is held while forking so that a VM that exits immediately is
  // only reaped after its pid has been recorded.
  pthread_mutex_lock(&dc->reservations_lock);
  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) {
    pthread_mutex_unlock(&dc->reservations_lock);
    return 1;
  }

  int ret = spawn_all_vms(res);
  res->state = RES_STATE_RUNNING;
  pthread_mutex_unlock(&dc->reservations_lock);

  return ret;
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  datacenter_reap_vms(dc);

  // Takes every lock (in the global order) to print a consistent snapshot, and
  // locks stdout so the listing is not interleaved with other threads' output.
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  pthread_mutex_lock(&dc->reservations_lock);
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_lock(&dc->servers[i].lock);
  }
  flockfile(stdout);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server server = dc->servers[i];

    Resources used = server.total;
    resources_sub(&used, server.available);

    printf(" Server #%zu\n", server.id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.total.cpu, server.total.ram, server.total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.available.cpu, server.available.ram, server.available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server.num_hosted_vms);

    if (server.num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server.num_hosted_vms; j++) {
        VM *vm = server.hosted_vms[j];
        const char *prefix = (j == server.num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = *dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");

  funlockfile(stdout);
  for (size_t i = dc->num_servers; i > 0; i--) {
    pthread_mutex_unlock(&dc->servers[i - 1].lock);
  }
  pthread_mutex_unlock(&dc->reservations_lock);
  pthread_rwlock_unlock(&dc->vm_types_lock);
  return 0;
}

/**
 * Polls for any terminated VM child processes using waitpid(..., WNOHANG)
 * and cleans up their resources immediately.
 *
 * @param dc Pointer to Data Center.
 */
void datacenter_reap_vms(DataCenter *dc) {
  if (dc == NULL || !dc->configured) return;

  pid_t pid;
  int status;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    pthread_mutex_lock(&dc->reservations_lock);
    int found = 0;
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation *res = dc->reservations[i];
      if (res->state != RES_STATE_RUNNING) continue;
      for (size_t j = 0; j < res->num_vms; j++) {
        VM *vm = res->vms[j];
        if (vm != NULL && vm->pid == pid) {
          vm_destroy(dc, vm);
          found = 1;
          break;
        }
      }
      if (found) break;
    }
    pthread_mutex_unlock(&dc->reservations_lock);
  }
}

/**
 * Checks if the Data Center currently has any running VMs.
 *
 * @param dc Pointer to Data Center.
 * @return 1 if running VMs exist, 0 otherwise.
 */
int datacenter_has_running_vms(DataCenter *dc) {
  if (dc == NULL || !dc->configured) return 0;

  int running = 0;
  pthread_mutex_lock(&dc->reservations_lock);
  for (size_t i = 0; i < dc->num_reservations; i++) {
    if (dc->reservations[i]->state == RES_STATE_RUNNING && dc->reservations[i]->num_vms > 0) {
      running = 1;
      break;
    }
  }
  pthread_mutex_unlock(&dc->reservations_lock);
  return running;
}

/**
 * Suspends execution for delay_ms while periodically checking for
 * terminated child VMs and freeing their resources.
 *
 * @param dc Pointer to Data Center.
 * @param delay_ms Time to wait in milliseconds.
 */
void datacenter_wait_dc(DataCenter *dc, unsigned int delay_ms) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (1) {
    if (dc != NULL) {
      datacenter_reap_vms(dc);
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed_ms >= (long)delay_ms) {
      break;
    }
    long remaining_ms = (long)delay_ms - elapsed_ms;
    long step_ms = (remaining_ms > 20) ? 20 : remaining_ms;
    struct timespec step = {(time_t)(step_ms / 1000), (long)((step_ms % 1000) * 1000000)};
    nanosleep(&step, NULL);
  }

  if (dc != NULL) {
    datacenter_reap_vms(dc);
  }
}

void datacenter_wait(unsigned int delay_ms){
  datacenter_wait_dc(g_current_dc, delay_ms);
}