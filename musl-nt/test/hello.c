#include <unistd.h>

int main(void)
{
    static const char message[] = "hello musl-nt\n";
    return write(1, message, sizeof message - 1) != (ssize_t)(sizeof message - 1);
}
