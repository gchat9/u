#include "../_sys/_main.h"

#define MAX_PATH 4096
#define MAX_ITER 50  // Prevent infinite loops on circular symlinks

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    const char *path = NULL;
    int follow = 0;
    int status = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (!path && argv[i][0] == '-' && argv[i][1] != '-') {
            if (argv[i][1] == 'f' && argv[i][2] == '\0') {
                follow = 1;
            } else {
                exit(1);
                __builtin_unreachable();
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == '\0') {
            // "--" marks end of options
            if (i + 1 < argc) {
                path = argv[i + 1];
            }
            break;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        exit(1);
        __builtin_unreachable();
    }

    char buf[MAX_PATH];
    long n;

    if (follow) {
        // Recursively follow symlinks until we reach a non-symlink or max iterations
        size_t path_len = strlen(path);
        if (path_len >= MAX_PATH) {
            exit(1);
            __builtin_unreachable();
        }
        
        // Copy path into buffer for manipulation
        strlcpy(buf, path, MAX_PATH);

        for (int iter = 0; iter < MAX_ITER; iter++) {
            char target[MAX_PATH];
            n = readlink(buf, target, MAX_PATH - 1);
            if (n < 0) {
                // Not a symlink or error - print current resolved path
                break;
            }
            target[n] = '\0';

            // If target is absolute, use it directly; otherwise resolve relative to dir
            if (target[0] == '/') {
                strlcpy(buf, target, MAX_PATH);
            } else {
                // Find last '/' in current path to get directory
                char *last_slash = NULL;
                for (char *p = buf; *p; p++) {
                    if (*p == '/') last_slash = p;
                }
                
                if (last_slash) {
                    // Replace everything after last slash with target
                    *(last_slash + 1) = '\0';
                    strlcat(buf, target, MAX_PATH);
                } else {
                    // No directory component, just use target
                    strlcpy(buf, target, MAX_PATH);
                }
            }
        }
        
        // Print the final resolved path
        write(1, buf, strlen(buf));
        write(1, "\n", 1);
    } else {
        // Non-recursive: just read the symlink once
        n = readlink(path, buf, MAX_PATH - 1);
        if (n < 0) {
            exit(1);
            __builtin_unreachable();
        }
        buf[n] = '\0';
        write(1, buf, strlen(buf));
        write(1, "\n", 1);
    }

    exit(status);
    __builtin_unreachable();
}
