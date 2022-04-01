#include <stdio.h>
#define SANITIZER_API_DEBUG 1
#if SANITIZER_API_DEBUG
#define PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define PRINT(...)
#endif

#define PRINT_ERR(...) fprintf(stderr, __VA_ARGS__)
#define PRINT_INFO(...) fprintf(stderr, __VA_ARGS__)
