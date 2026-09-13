# PAC stripping

Ammonia does not strip PAC. Infect is the launchd payload, and PAC stripping must never run in PID 1 — a PAC fault there panics the kernel. Arm64e apps run natively instead: build with `BUILD_ARM64E=ON` and boot with `-arm64e_preview_abi`.

The old Plugin Playground rewriter (`exe.c` / `fangs_hook.c`, which rewrote arm64e app copies under `/tmp/RuntimeApplications/` when `disablePAC` was set and a spawn hook called `getready_process()`) has been removed. `disablePAC` remains a readable/writable option key for compatibility, but nothing acts on it.
