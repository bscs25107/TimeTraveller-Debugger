// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)
#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"

using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
inline bool readI32(FILE* f, int32_t& val)
{
    return fread(&val, sizeof(int32_t), 1, f) == 1;
}

inline bool readI64(FILE* f, int64_t& val)
{
    return fread(&val, sizeof(int64_t), 1, f) == 1;
}

static bool readString(FILE* f, string& outStr)
{
    int32_t len = 0;
    if (!readI32(f, len) || len < 0 || len > 4096) return false;
    if (len == 0) { outStr.clear(); return true; }
    outStr.resize(len);
    return fread(&outStr[0], 1, len, f) == (size_t)len;
}

static bool readVariable(FILE* f, Variable& v)
{
    return readString(f, v.name) && readI32(f, v.value);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct TdbgReader
{
    FILE* file;
    TTDBHeader header;
    vector<int64_t> index;
};

struct DebugSession
{
    bool active;
    TdbgReader reader;
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r <= 0)
        {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        got += (size_t)r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0)
        {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        sent += (size_t)r;
    }
    return true;
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    sockaddr_in addr{};
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        close(s);
        return -1;
    }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    return s;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE* f = fopen(sourcePath, "rb");
    if (!f) return false;

    Crc32 crc;
    char status = 0;
    crc.update(&status, 1);
    if (!sendAll(sock, &status, 1)) { fclose(f); return false; }

    char buf[IO_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        crc.update(buf, n);
        if (!sendAll(sock, buf, n)) { fclose(f); return false; }
    }
    fclose(f);

    char trailer[4];
    crcToBytes(crc.value(), trailer);
    if (!sendAll(sock, trailer, 4)) return false;

    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    char statusByte;
    if (!recvAll(sock, &statusByte, 1))
    {
        outError = "Socket closed, no reply";
        return false;
    }

    FILE* outFile = nullptr;
    if (statusByte == 0) outFile = fopen(outTdbgPath, "wb");

    TrailerChecker checker;
    char rawBuf[IO_BUFFER_SIZE];
    char releaseBuf[IO_BUFFER_SIZE + 4];
    string payloadErr = "";
    ssize_t n;

    while ((n = recv(sock, rawBuf, sizeof(rawBuf), 0)) > 0)
    {
        size_t released = checker.feed(rawBuf, (size_t)n, releaseBuf);
        if (released > 0)
        {
            if (statusByte == 0 && outFile) fwrite(releaseBuf, 1, released, outFile);
            else payloadErr.append(releaseBuf, released);
        }
    }

    if (outFile) fclose(outFile);

    if (!checker.verify())
    {
        if (statusByte == 0) remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }

    if (statusByte != 0)
    {
        outError = payloadErr;
        return false;
    }

    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================
//static bool readString(FILE* f, string& s)
//{
//    int32_t len;
//    if (!readI32(f, len) || len < 0 || len > 4096)
//    {
//        return false;
//    }
//    s.assign((size_t)len, '\0');
//    if (len > 0 && fread(&s[0], 1, (size_t)len, f) != (size_t)len)
//    {
//        return false;
//    }
//    return true;
//}
//static bool readVariable(FILE* f, Variable& v)
//{
//    return readString(f, v.name) && readI32(f, v.value);
//}
//////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
bool openTdbg(const char *path, TdbgReader &r)
{
    r.file = fopen(path, "rb");
    if (!r.file) return false;

    TTDBHeader& h = r.header;
    if (fread(h.magic, 1, 4, r.file) != 4 ||
        !readI32(r.file, h.version) ||
        !readI32(r.file, h.stepCount) ||
        !readI64(r.file, h.indexOffset))
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (memcmp(h.magic, "TTDB", 4) != 0 || h.stepCount < 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    r.index.resize(h.stepCount);
    if (fseek(r.file, (long)h.indexOffset, SEEK_SET) != 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (fread(r.index.data(), sizeof(int64_t), (size_t)h.stepCount, r.file) != (size_t)h.stepCount)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    if (r.file)
    {
        fclose(r.file);
        r.file = nullptr;
    }
    r.index.clear();
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (!r.file || step < 0 || step >= r.header.stepCount) return nullptr;
    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0) return nullptr;

    Snapshot* s = new Snapshot();
    bool ok = readI32(r.file, s->stackDepth) && s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;

    for (int i = 0; i < s->stackDepth && ok; i++)
    {
        Frame& fr = s->callStack[i];
        ok = readString(r.file, fr.func_name) && readI32(r.file, fr.argc) && fr.argc >= 0 && fr.argc <= MAX_VARS_PER_FRAME;
        for (int j = 0; j < fr.argc && ok; j++)
        {
            ok = readVariable(r.file, fr.argv[j]);
        }
        ok = ok && readI32(r.file, fr.returnLine) && readI32(r.file, fr.localCount) && fr.localCount >= 0 && fr.localCount <= MAX_VARS_PER_FRAME;
        for (int j = 0; j < fr.localCount && ok; j++)
        {
            ok = readVariable(r.file, fr.locals[j]);
        }
    }

    if (!ok)
    {
        delete s;
        return nullptr;
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active = false;
    s.reader.file = nullptr;
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active)
    {
        closeTdbg(s.reader);
        s.active = false;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot* snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (!snap)
    {
        cout << "damaged .tdbg file\n";
        return false;
    }
    printSnapshot((int32_t)s.currentStep, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active)
    {
        cout << "No active debug session. Type 'start <source.bin>'\n";
        return false;
    }
    int64_t target = s.currentStep + delta;
    if (target < 0) 
    {
        target = 0;
    }
    if (target >= s.reader.header.stepCount) 
    {
        target = s.reader.header.stepCount - 1;
    }

    s.currentStep = target;
    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (sock < 0)
    {
        cout << "Error: Could not connect to load balancer at " << LB_HOST << ":" << LB_PORT << "\n";
        return false;
    }

    if (!uploadSource(sock, sourcePath.c_str()))
    {
        cout << "Error: Failed uploading source file.\n";
        close(sock);
        return false;
    }

    string outErr;
    if (!receiveResponse(sock, TDBG_FILE, outErr))
    {
        cout << "Error: " << outErr << "\n";
        close(sock);
        return false;
    }
    close(sock);

    TdbgReader newReader;
    if (!openTdbg(TDBG_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file\n";
        return false;
    }

    if (s.active) 
    {
        closeTdbg(s.reader);
    }

    s.reader = newReader;
    s.currentStep = 0;
    s.active = true;

    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession& s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "--- STEP " << (step + 1) << " / " << totalSteps << " ---\n";
    for (int i = s.stackDepth - 1; i >= 0; i--)
    {
        const Frame& f = s.callStack[i];
        cout << "[" << f.func_name << "]";
        for (int a = 0; a < f.argc; a++) 
        {
            cout << " " << f.argv[a].name << "=" << f.argv[a].value;
        }
        if (f.localCount > 0) 
        {
            cout << " | locals:";
        }
        for (int l = 0; l < f.localCount; l++)
        {
            cout << " " << f.locals[l].name << "=" << f.locals[l].value;
        }
        cout << "\n";
    }
}
void printHelp()
{
    // Print the commands
    cout << "Commands:\n"
        << "  start <file>        - upload file and start debugging session\n"
        << "  next                - step forward 1 step\n"
        << "  prv                 - step backward 1 step\n"
        << "  jmp next <n>        - jump forward n steps\n"
        << "  jmp prv <n>         - jump backward n steps\n"
        << "  help                - print this menu\n"
        << "  quit | exit         - exit debugger\n" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.

    istringstream iss(line);
    vector<string> words;
    string w;
    while (iss >> w) 
    {
        words.push_back(w);
    }

    if (words.empty()) 
    {
        return true;
    }
    if (words[0] == "ttdb") 
    {
        words.erase(words.begin());
    }
    if (words.empty())
    {
        return true;
    }

    string cmd = words[0];

    if (cmd == "start")
    {
        if (words.size() < 2) 
        {
            cout << "Usage: start <source_path>\n"; 
            return false;
        }
        return cmdStart(s, words[1]);
    }
    else if (cmd == "next")
    {
        return cmdNext(s);
    }
    else if (cmd == "prv") 
    {
        return cmdPrv(s);
    }
    else if (cmd == "jmp")
    {
        if (words.size() < 3) 
        { 
            cout << "Usage: jmp next/prv <n>\n";
            return false;
        }

        int n = atoi(words[2].c_str());
        if (n <= 0) 
        {
            return false;
        }
        if (words[1] == "next") 
        {
            return cmdJmpNext(s, n);
        }
        else if (words[1] == "prv")
        {
            return cmdJmpPrv(s, n);
        }
    }
    else if (cmd == "help") 
    { 
        printHelp();
        return true;
    }
    else if (cmd == "quit" || cmd == "exit") 
    { 
        quitRequested = true; 
        return true; 
    }

    cout << "unknown command\n";
    printHelp();
    return false;

}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

    if (argc >= 2) 
    {
        LB_HOST = argv[1];
    }
    if (argc >= 3) 
    {
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "Welcome to Time-Travel Debugger (TTDB) Client\n";
    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested)
    {
        cout << PROMPT;
        if (!getline(cin, line)) 
        {
            break;
        }
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;

}