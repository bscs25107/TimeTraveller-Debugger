#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("Usage: ttdb <source.bin>\n");
        return 1;
    }
    std::printf("Input file: %s\n", argv[1]);
    return 0;
}
