#include <stdio.h>
#include <string.h>
#include<stdlib.h>
#include<unistd.h>
#include <stdlib.h>


#define MSG "Hello World"


int main(void)
{
    FILE *f;
    char *args[] = {"/bin/cat", "test_execve1.c",NULL};
    char *envp[] = {"LD_PRELOAD=/nobackup/sarvi/filesystem_tracker/src/libwisktrack.so", NULL};

    printf("\nTest Case: test_execve1\n");
    printf("Test Case PID: %u\n", getpid());
    printf("LD_PRELOAD: %s\n", getenv("LD_PRELOAD"));
    printf("WISK_TRACKER_DEBUGLEVEL: %s\n", getenv("WISK_TRACKER_DEBUGLEVEL"));
    printf("WISK_TRACKER_PIPE: %s\n", getenv("WISK_TRACKER_PIPE"));
    printf("WISK_TRACKER_UUID: %s\n", getenv("WISK_TRACKER_UUID"));
    f = fopen("/tmp/testfile1", "w");
    fprintf(f, MSG);
    fclose(f);
    int ret = system("export WISK_TRACKER_DEBUGLEVEL=3; export WISK_TRACKER_UUID=casaASAS; export WISK_TRACKER_PIPE=/tmp/wisk_tracker.pipe; export LD_PRELOAD=/nobackup/sarvi/filesystem_tracker/src/libwisktrack.so; /bin/cat test_execve1.c");

    printf("\nsub command complete\n");
    return 0;
}
