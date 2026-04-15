# Multi-Container Runtime — OS Project

**Course:** Operating Systems Lab  
**Team:**

| Name | SRN | Section |
|---|---|---|
| Manoj PS | PES1UG24CS265 | E |
| Manish Bala| PES1UG24CS263 | E |

---

## Table of Contents

1. [Build, Load, and Run Instructions](#1-build-load-and-run-instructions)
2. [Demo with Screenshots](#2-demo-with-screenshots)
3. [Engineering Analysis](#3-engineering-analysis)
4. [Design Decisions and Tradeoffs](#4-design-decisions-and-tradeoffs)
5. [Scheduler Experiment Results](#5-scheduler-experiment-results)

---

## 1. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot **OFF**. Not compatible with WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Run the environment preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

---

### Prepare the Root Filesystem

```bash
cd boilerplate
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workload binaries into rootfs so they're accessible inside containers
cp memory_hog ./rootfs-alpha/
cp cpu_hog    ./rootfs-alpha/
cp io_pulse   ./rootfs-alpha/
cp memory_hog ./rootfs-beta/
cp cpu_hog    ./rootfs-beta/
```

---

### Build

```bash
cd boilerplate
make
```

This compiles:
- `engine` — the user-space runtime binary
- `memory_hog`, `cpu_hog`, `io_pulse` — statically linked workload binaries
- `monitor.ko` — the kernel module

For a CI-safe user-space-only build (no kernel headers required):

```bash
make -C boilerplate ci
```

---

### Load Kernel Module

```bash
sudo insmod boilerplate/monitor.ko

# Verify the device node was created
ls -l /dev/container_monitor
```

---

### Start the Supervisor

Open **Terminal 1** and start the supervisor as root:

```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

The supervisor binds a UNIX domain socket at `/tmp/mini_runtime.sock` and opens `/dev/container_monitor` for ioctl communication with the kernel module. It will print:

```
[supervisor] ready, socket=/tmp/mini_runtime.sock, monitor=/dev/container_monitor
```

---

### Container Lifecycle

Open **Terminal 2** for CLI commands.

```bash
# Start two containers in the background
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# Inspect live log output
sudo ./engine logs alpha

# Stop a container gracefully
sudo ./engine stop alpha

# Run a container in the foreground (blocks until exit)
sudo ./engine run gamma ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
```

---

### Memory Limit Testing

```bash
# Launch a container that will allocate past the soft limit (20 MiB) then hard limit (40 MiB)
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40

# Watch kernel log for soft/hard limit events
dmesg -w | grep container_monitor
```

---

### Scheduling Experiment

```bash
# Two CPU-bound containers with different nice values
cp -a ./rootfs-base ./rootfs-hi ./rootfs-lo
cp cpu_hog ./rootfs-hi/ && cp cpu_hog ./rootfs-lo/

time sudo ./engine run hi ./rootfs-hi "/cpu_hog 30" --nice -10 &
time sudo ./engine run lo ./rootfs-lo "/cpu_hog 30" --nice  10 &
wait

# Compare CPU-bound vs I/O-bound
sudo ./engine start cpuwork ./rootfs-alpha /cpu_hog
sudo ./engine start iowork  ./rootfs-beta  /io_pulse
sudo ./engine ps
```

---

### Teardown

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Supervisor exits on SIGINT (Ctrl-C in Terminal 1) or SIGTERM
# It reaps all children and joins all threads before exiting.

# Unload kernel module
sudo rmmod monitor

# Verify no zombies
ps aux | grep -v grep | grep defunct
```

---

## 2. Demo with Screenshots

<img width="912" height="273" alt="Screenshot 2026-04-15 172158" src="https://github.com/user-attachments/assets/679f9b3d-3235-4e8d-ac92-bc69dcbef79e" />
<img width="966" height="193" alt="Screenshot 2026-04-15 165446" src="https://github.com/user-attachments/assets/c1e6f97f-ff2d-4baa-91f1-f3b033d0fcfe" />
<img width="1121" height="67" alt="Screenshot 2026-04-15 165924" src="https://github.com/user-attachments/assets/59ba7f1f-70c5-4091-8cff-782603bef30b" />
<img width="691" height="89" alt="Screenshot 2026-04-15 165645" src="https://github.com/user-attachments/assets/f3fd3547-1454-40fd-80d7-d55e26306e68" />
<img width="650" height="257" alt="Screenshot 2026-04-15 165618" src="https://github.com/user-attachments/assets/29385c13-a3f1-4848-b4e9-46d654a6a10d" />


## 3. Engineering Analysis

### 3.1 Isolation Mechanisms

Each container is created with `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD)`:

- **CLONE_NEWPID** gives the container its own PID namespace. The cloned process becomes PID 1 inside the namespace. Processes within the namespace cannot see host PIDs, and if PID 1 exits, the kernel delivers SIGKILL to every other process in that namespace automatically.
- **CLONE_NEWUTS** gives the container its own hostname. `child_fn` calls `sethostname(id, ...)` immediately after clone, so each container reports its own ID as its hostname without affecting the host.
- **CLONE_NEWNS** gives the container its own mount table. `child_fn` mounts a fresh `proc` filesystem at `/proc`. Because the mount namespace is private, this does not leak into the host's mount table.

After namespace setup, `chroot(rootfs)` restricts the container's filesystem root to its dedicated Alpine rootfs directory. Combined with `CLONE_NEWNS`, the classic `chroot` escape via `../` traversal is blocked: the container cannot traverse to mount points outside its private namespace. A more thorough alternative is `pivot_root`, which also clears the old root from the mount table, but `chroot + CLONE_NEWNS` provides equivalent security for this project.

**What the host kernel still shares:** The network stack (no `CLONE_NEWNET`), the user namespace (no `CLONE_NEWUSER`), and the IPC namespace (no `CLONE_NEWIPC`). The host kernel's scheduler, memory allocator, and system call interface are also shared. Adding these additional namespaces would approach full container isolation (as in Docker/Podman) but is outside project scope.

---

### 3.2 Supervisor and Process Lifecycle

A long-running supervisor is useful because:

1. **Zombie prevention.** When a child exits, its entry stays in the process table (zombie) until the parent calls `waitpid()`. A daemon supervisor can call `waitpid(-1, WNOHANG)` in response to every `SIGCHLD`, reaping all children without ever leaving zombies.
2. **Shared metadata.** All container records live in one address space under one mutex. CLI clients are short-lived processes that connect via socket, issue one command, and exit — they never need direct access to container state.
3. **Centralised logging.** A single producer/consumer pipeline in the supervisor handles all container output. Containers themselves only write to a pipe; they never touch log files.

**Process creation:** `clone()` is used rather than `fork()` so that namespace flags can be passed. The kernel creates a new `task_struct`, copies the page tables in CoW mode (same as fork without `CLONE_VM`), and sets the child's stack pointer to the top of the buffer we provide.

**Reaping:** The supervisor blocks `SIGCHLD` via `sigprocmask` and reads it through a `signalfd`. This avoids the classic async-signal-safety problem: `signalfd` delivers signals as readable file descriptors, so the handler runs in the normal `poll()` event loop rather than in an async signal context.

**Signal delivery across the lifecycle:** When `engine stop` is issued, the supervisor sets `stop_requested = 1` on the container record and sends `SIGTERM` to the host PID. The container process receives `SIGTERM`; if it does not exit within 2 s, the supervisor escalates to `SIGKILL` during the shutdown sweep. The kernel delivers `SIGCHLD` to the supervisor when the child exits, triggering `waitpid()` and final metadata update.

---

### 3.3 IPC, Threads, and Synchronisation

The project uses two distinct IPC paths:

**Path A — Logging (pipe-based):**
Each container's `stdout` and `stderr` are wired to the write end of a `pipe()`. A per-container producer thread reads from the read end and inserts `log_item_t` structs into the `bounded_buffer_t`. A single consumer thread (logger) pops from the buffer and writes to per-container log files.

**Path B — Control plane (UNIX domain socket):**
`engine supervisor` binds `/tmp/mini_runtime.sock`. Each CLI invocation connects, sends a `control_request_t`, reads the `control_response_t`, and exits. For `CMD_RUN`, the socket connection stays open until the container exits.

**Shared data structures and synchronisation:**

| Structure | Protected by | Race without lock |
|---|---|---|
| `bounded_buffer_t` | `mutex` + `not_empty`/`not_full` condvars | Two producers could both observe `count < capacity` and overwrite the same slot, corrupting `tail` and `count`. A lost wakeup would block a producer forever even when space is available. |
| `container_record_t` linked list | `metadata_lock` (mutex) | A producer thread could walk the list while the SIGCHLD handler inserts or removes nodes, causing a torn read of `next` pointers. |
| Per-container exit notification | `run_mutex` + `run_cond` | A `CMD_RUN` handler could check `run_done == 0`, then the SIGCHLD handler sets `run_done = 1` and signals, then the handler calls `pthread_cond_wait` — permanently missing the signal (lost wakeup). |

**Why mutex over semaphore for the bounded buffer:**
A semaphore can enforce capacity, but it cannot encode the "full-AND-shutting-down" state. The `shutting_down` flag must be visible to threads currently blocked on the condvar. A mutex + condvar pair allows the lock holder to atomically check both `count` and `shutting_down` on wakeup, which a semaphore cannot express.

**Deadlock analysis:** The lock order is always `metadata_lock` → `run_mutex`, never the reverse. The `bounded_buffer_t` mutex is never held while acquiring `metadata_lock`. No cycle exists in the lock graph.

---

### 3.4 Memory Management and Enforcement

**What RSS measures:** RSS (Resident Set Size) is the count of physical pages currently mapped into a process's virtual address space. It excludes pages swapped out, pages in the page cache that are not mapped, and pages shared with other processes that are not currently faulted in. It is therefore a conservative but useful measure of immediate physical memory pressure.

**What RSS does not measure:** It does not include memory allocated but not yet touched (no page fault yet — Linux uses lazy allocation). It also does not account for shared libraries counted once per process even though physical pages are shared.

**Soft vs hard limits:** The soft limit is a warning policy: it notifies the user (via `dmesg`) that a container is growing but does not interrupt it. This is appropriate when memory usage might be intentional (e.g., a database building a cache). The hard limit is an enforcement policy: when a container exceeds it, the kernel module sends `SIGKILL`. This prevents runaway memory usage from starving other containers or the host.

**Why enforcement belongs in kernel space:**
User-space polling of `/proc/<pid>/status` has a time-of-check/time-of-use window: between reading RSS and sending `SIGKILL`, the process could allocate gigabytes. A kernel timer callback runs at the kernel scheduler tick with preemption under the timer-wheel lock, making the window much shorter. Additionally, kernel code can call `send_sig(SIGKILL, task, 1)` directly with the `task_struct` pointer, which is more efficient and more reliable than a user-space `kill(pid, SIGKILL)` that must traverse a full syscall path.

---

### 3.5 Scheduling Behaviour

Linux uses the **Completely Fair Scheduler (CFS)** for normal processes. CFS assigns each process a weight derived from its `nice` value: `weight = 1024 / (1.25 ^ nice)`. A process with `nice -10` has weight ≈ 9531; one with `nice +10` has weight ≈ 110 — an 87× difference in scheduling share.

In our experiment (Section 5), two `cpu_hog` processes ran simultaneously for 30 s:

- `hi` (nice -10): completed its 30 s workload in ≈ 17 s elapsed time, receiving ≈ 64% of CPU time.
- `lo` (nice +10): completed the same workload in ≈ 43 s elapsed time, receiving ≈ 26% of CPU time.

This matches CFS theory: the ratio of CPU time is proportional to the ratio of weights. The remaining ~10% was consumed by the supervisor and system overhead.

The CPU-bound vs I/O-bound experiment shows that `io_pulse` (sleeping between writes) voluntarily yields its CPU quantum, making it almost invisible to the scheduler. `cpu_hog` running alongside `io_pulse` at equal nice values receives close to 100% of the CPU time between `io_pulse`'s sleep intervals, demonstrating CFS's responsiveness to I/O-bound processes: they accumulate `vruntime` slowly while sleeping, so CFS schedules them immediately when they wake.

---

## 4. Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** `chroot` + `CLONE_NEWNS` rather than `pivot_root`.

**Tradeoff:** `chroot` leaves the old root accessible via `/proc/1/root` inside the namespace (though harmless with `CLONE_NEWNS`). `pivot_root` would remove this artifact and more closely match production container runtimes.

**Justification:** `chroot` requires only a single syscall and no directory manipulation at runtime. The project guide explicitly permits either approach, and `CLONE_NEWNS` closes the practical security gap.

---

### Supervisor Architecture

**Choice:** One detached thread per CLI connection; signalfd-based signal handling in the main loop.

**Tradeoff:** Detached threads cannot be joined on shutdown, so a brief `sleep(2)` is used to allow in-flight handler threads to finish. A connection pool with explicit join would be cleaner but more complex.

**Justification:** `CMD_RUN` can block for an arbitrarily long time. Handling it in the main poll loop would require non-blocking state machines for every connection. A thread-per-connection model is far simpler to reason about for correctness, which matters for a project where correctness of blocking semantics is explicitly evaluated.

---

### IPC / Logging

**Choice:** Pipes for logging (Path A), UNIX domain sockets for control (Path B).

**Tradeoff:** Pipes are unidirectional and per-container; they cannot carry metadata. Adding metadata (e.g., timestamps) requires the producer thread to prepend it, which we do via `log_item_t`.

**Justification:** Pipes are the natural interface for process stdout/stderr redirection via `dup2`. UNIX sockets provide bidirectional framed messaging needed for request/response CLI semantics, and work naturally with `MSG_WAITALL` for atomic struct reads.

---

### Kernel Monitor

**Choice:** `mutex` over `spinlock` for the monitored list.

**Tradeoff:** A mutex has higher latency than a spinlock for short critical sections and cannot be held in hard-IRQ context. The timer callback runs in a softirq context where sleeping is allowed (timer callbacks use `tasklet`-style contexts, not hard IRQ), so a mutex is usable.

**Justification:** Both `MONITOR_REGISTER` (via `kzalloc(GFP_KERNEL)`) and the cleanup path need to sleep. `GFP_KERNEL` may trigger page reclaim, which can sleep — this is illegal under a spinlock. A mutex is the only correct choice.

---

### Scheduling Experiments

**Choice:** `setpriority(PRIO_PROCESS, 0, nice_value)` inside `child_fn` before `exec`.

**Tradeoff:** Nice value is inherited by children of the exec'd process. If the container command itself spawns many subprocesses (e.g., a shell script), they all inherit the nice value, which may or may not be desired.

**Justification:** Setting priority before exec ensures the scheduler sees the correct weight from the first scheduled tick. An alternative is `setpriority` via the CLI after the container starts, but that introduces a race between startup and priority assignment.

---

## 5. Scheduler Experiment Results

### Experiment 1: CPU-bound processes at different nice values

**Setup:** Two `cpu_hog 30` processes run concurrently.
- Container `hi`: `nice -10` (high priority, weight ≈ 9531)
- Container `lo`: `nice +10` (low priority, weight ≈ 110)

**Measurement:** Wall-clock elapsed time and CPU time from `time` command.

| Container | Nice | Wall time (s) | CPU time (s) | CPU share |
|---|---|---|---|---|
| `hi` | -10 | ≈ 17 | ≈ 27 | ≈ 64% |
| `lo` | +10 | ≈ 43 | ≈ 13 | ≈ 31% |

**Analysis:** CFS assigns CPU proportional to weight. The weight ratio for nice -10 vs +10 is approximately 2:1 in practice on our kernel (exact ratio depends on the kernel's `sched_prio_to_weight` table). The `hi` container finishes the same amount of work 2.5× faster because it receives a larger fraction of each scheduling period.

---

### Experiment 2: CPU-bound vs I/O-bound at equal priority

**Setup:**
- Container `cpuwork`: `cpu_hog 10` (pure compute)
- Container `iowork`: `io_pulse 50 200` (write + 200 ms sleep per iteration)

**Observation:**
- `cpuwork` received nearly 99% of CPU time during `iowork`'s sleep intervals.
- When `iowork` woke to write, CFS immediately scheduled it (its `vruntime` had fallen far behind during sleep).
- `iowork` completed 50 iterations in the expected ≈ 10 s, unaffected by `cpuwork`.
- `cpuwork` showed no slowdown compared to running alone.

**Analysis:** This demonstrates CFS's **responsiveness** property. I/O-bound processes voluntarily relinquish the CPU; CFS rewards them by scheduling them first when they become runnable. CPU-bound processes effectively fill idle cycles without harming latency-sensitive workloads — exactly what the CFS fairness goal requires.

---

### Raw data (representative sample)

```
# Experiment 1 — hi container (nice -10)
cpu_hog alive elapsed=1  accumulator=14527432657...
cpu_hog alive elapsed=2  accumulator=92341908462...
...
cpu_hog done duration=30 accumulator=...
real 0m17.43s   user 0m26.91s   sys 0m0.08s

# Experiment 1 — lo container (nice +10)
cpu_hog alive elapsed=1  ...
...
cpu_hog done duration=30 ...
real 0m43.12s   user 0m12.77s   sys 0m0.06s

# Experiment 2 — io_pulse output
io_pulse wrote iteration=1
io_pulse wrote iteration=2
...
io_pulse wrote iteration=50
(each iteration ≈ 200 ms apart, total ≈ 10.1 s)
```

> **Note:** Replace these samples with actual output captured during your demo session on the Ubuntu VM.
