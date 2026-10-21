# cloudIST

This repository contains implementation for the cloudIST operating systems project. 
The program simulates a data center that manages servers, VM types, reservations, and VM execution commands.

## Project Structure

### Core source files

- `main.c`: Program entry point, command loop, and command-line configuration.
- `parser.c` / `parser.h`: Parsing of command-line values and commands read from
  standard input.
- `datacenter.c` / `datacenter.h`: Data center configuration, VM type
  registration, reservations, execution, listing, and waits.
- `datacenter_utils.c` / `datacenter_utils.h`: VM type and reservation lookup,
  reservation validation and allocation, cleanup, and VM lifecycle helpers.
- `resources.c` / `resources.h`: Resource operations for CPU, RAM, and disk.
- `filesystem.c` / `filesystem.h`: Filesystem checks and absolute-path
  resolution.
- `constants.h`: Limits used by the data center, reservations, VM types, and
  identifiers.

## Usage

```
cloudIST <SERVERS> <RAM> <DISK> <CPU> <LOCAL_CORES> [<INPUT_DIR> [<MAX_THREADS>]]
```

`LOCAL_CORES` is the number of cores of the local machine used by CloudIST.
Each VM runs under `cpulimit -l CPULIMIT`, where
`CPULIMIT = 100 * VM_vCPUs * LOCAL_CORES / (CPU * SERVERS)`, capped at
`100 * LOCAL_CORES`. Without `INPUT_DIR`, commands are read from stdin.

With `MAX_THREADS`, the `.conf` files in `INPUT_DIR` are processed in parallel
by a pool of at most `MAX_THREADS` worker threads (each one takes the next file
in alphabetical order).

## Synchronization

- `DataCenter.vm_types_lock` (rwlock): VM type list (many lookups, few `D`).
- `DataCenter.reservations_lock` (mutex): reservation list, reservations and
  VM state/pid.
- `Server.lock` (one mutex per server): available resources and hosted VMs.
  A reservation only locks the servers it uses (by ascending id), so
  reservations on different servers are made in parallel. Copying the input
  folders is done without holding any lock.

Lock order: `vm_types_lock` -> `reservations_lock` -> server locks -> stdout.
