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
            Node* n = new Node();
            n->data = val;
            n->next = top;
            top = n;
            count++;
        }
        else { cout << "'-' Stack Limit Reached '-'\n"; }
    }
    T pop()
    {
        if (!isEmpty())
        {
            Node* old = top;
            T val = old->data;
            top = top->next;
            delete old;
            count--;
            return val;
        }
        cout << "'-' Stack At 0 '-'\n";
        return T();
    }
    T& peek()
    {
            return empty;

    }
    bool isEmpty()
    {
        if (count <= 0)
        {
            return true;
        }
        return false;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* current = top;
        int32_t i = 0;

        while (current != nullptr && i < maxLen)
        {
            out[i++] = current->data;
            current = current->next;
        }
        return i;
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
    Timeline():stepCount(0)
    {
        head = tail = nullptr;
    }

    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* n = new TimelineNode();
        n->data = s;
        if (head == nullptr)
        {
            tail = head = n;
        }
        else
        {
            tail->next = n;
            n->prev = tail;
            tail = n;
        }
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
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
        return 1;
    }

    fclose(file);
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
Variable* get_variable(const std::string& name);
Variable* find_variable(Frame& f, const string& name)
{
    for (int32_t i = 0; i < f.argc; i++)
    {
        if (f.argv[i].name == name) 
        {
            return &f.argv[i];
        }
    }
    for (int32_t i = 0; i < f.localCount; i++)
    {
        if (f.locals[i].name == name) 
        {
            return &f.locals[i];
        }
    }
    return nullptr;
}

int32_t return_value(Frame& f, const string& text)
{
    if (text.empty())
    {
        return 0;
    }
    if (isdigit(text[0])||(text[0]=='-'&&text.size()>1))
    {
          return stoi(text);
    }
    Variable* value1 = get_variable(f, text);
    if (value1 == nullptr)
    {
        return 0;
    }
    else 
    {
        return value1->value ;
    }
}
Variable* get_variable(Frame& f, const string& name)
{
    Variable* v = find_variable(f, name);
    if (v != nullptr) 
    {
        return v;
    }
    if (f.localCount < MAX_VARS_PER_FRAME)
    {
        f.locals[f.localCount].name = name;
        f.locals[f.localCount].value = 0;
        return &f.locals[f.localCount++];
    }
    return nullptr;
}
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

    size_t  idx = 0;
    size_t length = line.length();
    int32_t count = 0;
    string words;
    while (idx < length)
    {
        while (idx < length && isspace(line[idx]))
        {
            idx++;
        }
        if (idx >= length)
        {
            break;
        }
        if (count > maxTokens) 
        { 
            cout << "'-' Token Capacity Reached '-'\n";
            break;
        }

        while (idx < length && !isspace(line[idx]))
        {
            words += line[idx];
            idx++;
        }
        if (count == 0) { tokens[count].type = KEYWORD; }
        else if (count == 1) { tokens[count].type = IDENTIFIER; }
        else{ tokens[count].type = PARAM; }
        tokens[count].text = words;
        count++;
    }
    return count;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action

    FILE* f = fopen(resolveBinPath, "rb");
    if (!f) { cout << "ERROR: cannot open resolve.bin\n"; return; }

    string line;
    fseek(f, static_cast<long>(mainOffset), SEEK_SET);
    if (readResolveRecord(f, line) == -1) { fclose(f); return; }

    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);

    while (!callStack.isEmpty())
    {
        int64_t recStart = ftell(f);                  
        int64_t off = readResolveRecord(f, line);     
        if (off == -1) break;

        Token tokens[MAX_TOKENS];
        int32_t n = tokenizeLine(line, tokens, MAX_TOKENS);
        if (n == 0) continue;

        Frame& cur = callStack.peek();
        const string kw = tokens[0].text;

        if (kw == "set")
        {
            if (n < 3) { cout << "ERROR: 'set' needs a name and a value\n"; break; }
            Variable* v = get_variable(cur, tokens[1].text);
            if (!v) { cout << "ERROR: too many variables in " << cur.func_name << "\n"; break; }
            v->value = return_value(cur, tokens[2].text);
        }
        else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
        {
            if (n < 3) { cout << "ERROR: '" << kw << "' needs two operands\n"; break; }
            Variable* v = get_variable(cur, tokens[1].text);
            if (!v) { cout << "ERROR: too many variables in " << cur.func_name << "\n"; break; }
            int32_t rhs = return_value(cur, tokens[2].text);

            if (kw == "add")      v->value += rhs;
            else if (kw == "sub") v->value -= rhs;
            else if (kw == "mul") v->value *= rhs;
            else
            {
                if (rhs == 0) { cout << "ERROR: division by zero in " << cur.func_name << "\n"; break; }
                v->value /= rhs;
            }
        }
        else if (kw == "call")
        {
            if (n < 2) { cout << "ERROR: 'call' needs a function name\n"; break; }
            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                cout << "ERROR: stack overflow while calling " << tokens[1].text << "\n";
                break;
            }

            fseek(f, static_cast<long>(off), SEEK_SET);
            string hdrLine;
            if (readResolveRecord(f, hdrLine) == -1) { cout << "ERROR: bad call target\n"; break; }
            Token hdr[MAX_TOKENS];
            int32_t nHdr = tokenizeLine(hdrLine, hdr, MAX_TOKENS);

            int32_t nParams = nHdr - 2; if (nParams < 0) nParams = 0;
            int32_t nArgs = n - 2;    if (nArgs < 0)   nArgs = 0;
            if (nParams != nArgs || nParams > MAX_VARS_PER_FRAME)
            {
                cout << "ERROR: wrong number of arguments in call to " << tokens[1].text << "\n";
                break;
            }

            Frame callee;
            callee.func_name = tokens[1].text;
            callee.argc = nParams;
            callee.localCount = 0;
            callee.returnLine = static_cast<int32_t>(recStart);  
            for (int32_t i = 0; i < nParams; i++)
            {
                callee.argv[i].name = hdr[2 + i].text;                        
                callee.argv[i].value = return_value(cur, tokens[2 + i].text);  
            }
            callStack.push(callee);     
        }
        else if (kw == "func_end")
        {
            Frame done = callStack.pop();   
            if (!callStack.isEmpty())
            {
                fseek(f, static_cast<long>(done.returnLine), SEEK_SET);
                string callLine;
                if (readResolveRecord(f, callLine) == -1) { cout << "ERROR: bad return address\n"; break; }
                Token ct[MAX_TOKENS];
                int32_t nct = tokenizeLine(callLine, ct, MAX_TOKENS);

                Frame& caller = callStack.peek();
                for (int32_t i = 0; i < done.argc && 2 + i < nct; i++)
                {
                    Variable* v = find_variable(caller, ct[2 + i].text);
                    if (v) v->value = done.argv[i].value;   
                }
            }
        }
        else
        {
            cout << "ERROR: unknown instruction '" << kw << "'\n";
            break;
        }

        timeline.record(buildSnapshot(callStack));  
    }

    fclose(f);
}

void write_header(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

static void write_string(FILE* f, const string& s)
{
    int32_t length = static_cast<int32_t>(s.length());
    fwrite(&length, sizeof(int32_t), 1, f);
    if (length > 0)
    {
        fwrite(s.data(), 1, s.length(), f);
    }
}

static void write_variable(FILE* f, const Variable& v)
{
    write_string(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);
}

static int32_t clamp_ct(int32_t c)
{
    if (c < 0) 
    {
        return 0;
    }
    if (c > MAX_VARS_PER_FRAME) 
    {
        return MAX_VARS_PER_FRAME;
    }
    return c;
}

static void write_frame(FILE* f, const Frame& fr)
{
    write_string(f, fr.func_name);

    int32_t arg_c = clamp_ct(fr.argc);
    fwrite(&arg_c, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < arg_c; i++) 
    {
        write_variable(f, fr.argv[i]);
    }

    fwrite(&fr.returnLine, sizeof(int32_t), 1, f);

    int32_t l = clamp_ct(fr.localCount);
    fwrite(&l, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < l; i++) 
    {
        write_variable(f, fr.locals[i]);
    }
}

static void write_snapshot(FILE* f, const Snapshot& s)
{
    int32_t size = s.stackDepth;
    if (size < 0)
    {
        size = 0;
    }
    if (size > MAX_STACK_DEPTH) 
    {
        size = MAX_STACK_DEPTH;
    }
    fwrite(&size, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < size; i++) 
    {
        write_frame(f, s.callStack[i]);
    }
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
        FILE* f = fopen(tdbgPath, "wb");
        if (!f) { cout << "ERROR:( cannot create " << tdbgPath << "\n"; return; }

        int32_t steps = timeline.getStepCount();

        // 1. placeholder header
        TTDBHeader h;
        h.magic[0] = 'T'; h.magic[1] = 'T'; h.magic[2] = 'D'; h.magic[3] = 'B';
        h.version = 1;
        h.stepCount = steps;
        h.indexOffset = 0;
        write_header(f, h);

        // 2. snapshots, remembering where each one starts
        int64_t* index = new int64_t[steps > 0 ? steps : 1];
        int32_t written = 0;
        for (TimelineNode* n = timeline.begin(); n != nullptr && written < steps; n = n->next)
        {
            index[written++] = ftell(f);
            write_snapshot(f, *n->data);
        }

        // 3. index array goes right after the last snapshot
        h.indexOffset = ftell(f);
        if (written > 0) fwrite(index, sizeof(int64_t), written, f);
        delete[] index;

        // 4. go back and fill in the real header
        h.stepCount = written;
        fseek(f, 0, SEEK_SET);
        write_header(f, h);

        fclose(f);
    
}



// main section
int32_t main()
{
    if (!validateProgram("source.bin")) return 1;

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0) return 1;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");
    return 0;
}