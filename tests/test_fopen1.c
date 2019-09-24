#include <stdio.h>
#include <string.h>

#define MSG "Hello World"

int main(void)
{
    FILE *f;

    printf("\nTest Case: test_fopen1\n");
    f = fopen("/tmp/testfile1", "w");
    write(f, MSG, strlen(MSG));
    fclose(f);
    f = fopen("/tmp/testfile1", "a");
    write(f, MSG, strlen(MSG));
    fclose(f);
    f = fopen("/tmp/testfile1", "r");
    write(f, MSG, strlen(MSG));
    fclose(f);
    return 0;
}
