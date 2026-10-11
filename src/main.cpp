// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive    -- stream the client's .trace bytes straight to source.bin on disk                                              HOGYAAAAAAAAAAA!!!!!!!!!!!!!!
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>/*
#include <unistd.h>
#include <sys/socket.h>*/
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };

    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack():count(0)
    { 
        top = nullptr;
    }
    void push(const T& val)
    {
        if (count < MAX_STACK_DEPTH)
        {
            top[count] = val;
            count++;
        }
        else { cout << "'-' Stack Limit Reached '-'\n"; }
    }
    T pop()
    {
        if (!isEmpty())
        {
        }
    }
    T& peek()
    {
        // returns the top value on the stack
    }
    bool isEmpty()
    {
    }
    int32_t depth()
    {
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
    }
    TimelineNode* begin()
    {
    }
    int32_t getStepCount()
    {
    }
};

// Core structs
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line
    string line;
    while (getline(in, line))
    {
        if (!line.empty()&&line.back() == '\r') { line.pop_back(); }
        if (line.find_first_not_of(" \t")==string::npos) { continue; }
        else
        {
            out = line;
            return true;
        }
    }
    return false;
}
string firstWord(const string& line)
{
    // returns first word from the input string
    string f_word;
    for (size_t i = 0;i < line.length();i++)
    {
        if (line[i] != ' ' && line[i] != '\t')
        {
            f_word += line[i];
        }
        if (f_word.length() != 0 && line[i] == ' ') { return f_word; }
        if (f_word.length() != 0 && line[i] == '\t') { return f_word; }
    }
    return f_word;
}
string secondWord(const string& line)
{
    // returns second word from the input string
    size_t i = 0;
    size_t len = line.length();

    while (i < len && isspace((line[i]))) { i++; }
    while (i < len && !isspace((line[i]))) { i++; }
    while (i < len && isspace((line[i]))) { i++; }

    string sword;
    while (i < len && !isspace((line[i]))) 
    {
        sword += line[i];
        i++;
    }

    return sword;
}


bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream f(sourcePath, ios::binary);
    if (!f) { cout << "ERROR:( file not found\n"; return false; }
    
    bool open = false;
    int line_no=0;
    int firstline = 0;
    string line;
    while (readSourceLine(f, line))
    {
        line_no++; 
        if ("func" == firstWord(line)) 
        {
            if (!open) { open = true; firstline = line_no;; }
            else { cout << "'-' Nested Functions '-'\n";return false; }
        }
        if ("func_end" == firstWord(line))
        {
            if (open) { open = false;  }
            else { cout << "'-' Missing func at line "<<line_no<<" '-'\n"; return false; }
        }
    }
    if (open) { cout << "'-' Missing Func_end at line" << firstline << " '-'\n"; return false; }
    else { return true; }
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t start = ftell(f);
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t text_length = static_cast<int32_t>(text.length());
    fwrite(&text_length, sizeof(int32_t), 1, f);
    if (!text.empty())
    {
        fwrite(text.data(), sizeof(char), text.size(), f);
    }
    return start;
}

int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, 
    // returns the offset field - the raw line text comes back untouched in outText.
    // returns this record's own starting byte position
    int64_t offsetField;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    int32_t text_length = static_cast<int32_t>(outText.length());
    if (fread(&text_length, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }
    outText.resize(text_length);
    if (text_length>0)
    {
        fread(&outText[0], sizeof(char), outText.size(), f);
    }
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{ // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath, ios::binary);
    if (!in) { cout << "ERROR: source file not found\n"; return -1; }

    FILE* file = fopen(resolveBinPath, "w+b");
    if (!file) { cout << "ERROR: cannot create resolve.bin\n"; return -1; }

    string line;
    while (readSourceLine(in, line))
    {
        string kw = firstWord(line);
        int64_t start = ftell(file);
        writeResolveRecord(file, start, line);

        if (kw == "func")
        {
            if (funcCount >= MAX_FUNCS)
            {
                cout << "ERROR: too many functions\n";
                fclose(file);
                return -1;
            }
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = start;
            funcCount++;
        }
        else if (kw == "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                cout << "ERROR: too many calls\n";
                fclose(file);
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = start;
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
    }

    for (int32_t j = 0; j < patchCount; j++)
    {
        int64_t target = -1;
        for (int32_t i = 0; i < funcCount; i++)
        {
            if (funcArray[i].funcName == patches[j].targetFuncName)
            {
                target = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }
        if (target < 0)
        {
            cout << "ERROR: call to undefined function '"
                << patches[j].targetFuncName << "'\n";
            fclose(file);
            return -1;
        }
        fseek(file, static_cast<long>(patches[j].byteOffsetOfOffsetField), SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, file);
    }

    int64_t mainOffset = -1;
    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }
    if (mainOffset < 0)
    {
        cout << "ERROR: no main function\n";
        fclose(file);
        return -1;
    }

    fclose(file);
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}



// main section
int32_t main()
{


    if (!validateProgram("source.bin"))
    {
       
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    cout << "main at " << mainOffset << endl;
    FILE* f = fopen("resolve.bin", "rb");
    string t;
    int64_t off;
    while ((off = readResolveRecord(f, t)) != -1)
        cout << off << " [" << t << "]" << endl;
    fclose(f);
    return 0;
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}