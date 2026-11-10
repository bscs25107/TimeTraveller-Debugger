#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <cstring>
using namespace std;

const int IO_BUFFER_SIZE = 64 * 1024;
const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct Crc32Table
{
    uint32_t t[256];
    Crc32Table()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
inline const uint32_t* crc32Table()
{
    static const Crc32Table table;
    return table.t;
}

class Crc32
{
    uint32_t state;

public:
    Crc32() : state(0xFFFFFFFFu) {}
    void update(const char* buf, size_t n)
    {
        const uint32_t* t = crc32Table();
        for (size_t i = 0; i < n; i++)
            state = t[(state ^ (uint8_t)buf[i]) & 0xFF] ^ (state >> 8);
    }
    uint32_t value() const { return state ^ 0xFFFFFFFFu; }
};

inline void crcToBytes(uint32_t v, char out[4])
{
    out[0] = (char)(v & 0xFF);
    out[1] = (char)((v >> 8) & 0xFF);
    out[2] = (char)((v >> 16) & 0xFF);
    out[3] = (char)((v >> 24) & 0xFF);
}
inline uint32_t bytesToCrc(const char in[4])
{
    return (uint32_t)(uint8_t)in[0] | ((uint32_t)(uint8_t)in[1] << 8) |
        ((uint32_t)(uint8_t)in[2] << 16) | ((uint32_t)(uint8_t)in[3] << 24);
}

class TrailerChecker
{
    char tail[4];
    int tailLen;
    Crc32 crc;

public:
    TrailerChecker() : tailLen(0) {}
    void absorb(const char* buf, size_t n) { crc.update(buf, n); }
    size_t feed(const char* in, size_t n, char* out)
    {
        memcpy(out, tail, tailLen);
        memcpy(out + tailLen, in, n);
        size_t total = (size_t)tailLen + n;
        size_t release = total > 4 ? total - 4 : 0;
        crc.update(out, release);
        memcpy(tail, out + release, total - release);
        tailLen = (int)(total - release);
        return release;
    }

    bool verify() const
    {
        if (tailLen != 4)
            return false;
        return bytesToCrc(tail) == crc.value();
    }

    const char* trailer() const { return tail; }
};