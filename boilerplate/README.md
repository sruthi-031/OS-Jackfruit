#  Mini Container Runtime (OS Jackfruit)

## Overview
This project implements a lightweight container runtime in C using Linux namespaces and a kernel module for memory monitoring.

It demonstrates core OS concepts like process isolation, IPC, and kernel-user interaction.

---

##  Features

- Multi-container execution
- Supervisor process (long-running)
- CLI support:
  - start
  - run
  - ps
  - stop
  - logs
- Namespace isolation:
  - PID
  - UTS
  - Mount
- `/proc` filesystem mounting
- Kernel memory monitoring
- Soft limit warning
- Hard limit enforcement (kills container)

---

##  Architecture

###  User Space
- `engine.c` → container runtime
- UNIX sockets → communication

###  Kernel Space
- `monitor.c` → memory tracking
- `/dev/container_monitor` → ioctl interface

---

##  How to Run

### Build
```bash
cd boilerplate
make
