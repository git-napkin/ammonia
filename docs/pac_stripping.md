# PAC stripping

`syphon/exe.c` can rewrite arm64e app copies under `/tmp/RuntimeApplications/` when `disablePAC` is set **and** a spawn hook calls `getready_process()`. Infect does not do that. Leave PAC strip out of launchd. Use `BUILD_ARM64E=ON` and `-arm64e_preview_abi`.

`fangs_hook.c` still has the rewriter if you ever load it somewhere other than PID 1.
