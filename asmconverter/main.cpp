// Invoke Converter - Win32 (Visual C++ 2022)
// Converts MASM "invoke" statements into push/call (x86)
// or register/stack setup + call (x64) instructions.
// Multiple lines are supported: every "invoke" line is converted,
// all other lines (comments, labels, other instructions, blank lines)
// are copied through unchanged.
//
// Options:
//   - x86 (MASM32) / x64 (ML64) output
//   - API name suffix: as is / A / W
//   - x86: "addr" of local variables -> lea + push
//   - x64: emit or omit "sub rsp / add rsp"
//   - Copy button (clipboard)
//   - Reverse mode: push/call (x86) or register setup + call (x64) -> invoke
//   - Multi-line input (Ctrl+Enter converts, Ctrl+A selects all)
//
// Project type: Windows Desktop Application / Empty Project, Unicode.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Force a GUI (non-console) program and the wide-char entry point wWinMain,
// no matter what the project settings say.
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define IDC_INPUT   1001
#define IDC_OUTPUT  1002
#define IDC_LABEL   1003
#define IDC_X86     1004
#define IDC_X64     1005
#define IDC_SUFFIX  1006
#define IDC_LOCALS  1007
#define IDC_FRAME   1008
#define IDC_COPY    1009
#define IDC_FWD     1010
#define IDC_REV     1011
// The Convert button uses IDOK so that Enter triggers it.

static const wchar_t* DEFAULT_LABEL = L"Invoke Converter  (x86 / x64)";
static const wchar_t* SAMPLE_FWD = L"invoke MessageBox,NULL,addr MsgText,NULL,NULL\r\ninvoke ExitProcess,0";
static const wchar_t* SAMPLE_REV = L"push 0h\r\npush 0h\r\npush OFFSET MsgText\r\npush 0h\r\ncall MessageBox\r\n\r\npush 0h\r\ncall ExitProcess";

static HWND   g_hInput, g_hOutput, g_hLabel, g_hButton, g_hCopy;
static HWND   g_hX86, g_hX64, g_hSuffix, g_hLocals, g_hFrame, g_hFwd, g_hRev;
static HFONT  g_hMono, g_hUI;
static HBRUSH g_hWhite;

struct Options
{
    bool x64;
    bool locals;    // x86: addr of local variable -> lea eax / push eax
    bool frame;     // x64: emit sub rsp / add rsp
    bool reverse;   // asm -> invoke
    int  suffix;    // 0 = as is, 1 = A, 2 = W
};

// ---------------------------------------------------------------- helpers
static std::wstring Trim(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Split on commas that are not inside quotes, [] () or <>.
static std::vector<std::wstring> SplitArgs(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    int depth = 0;
    wchar_t quote = 0;

    for (wchar_t c : s)
    {
        if (quote)
        {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == L'"' || c == L'\'') { quote = c; cur += c; continue; }
        if (c == L'(' || c == L'[' || c == L'<') ++depth;
        if (c == L')' || c == L']' || c == L'>') --depth;

        if (c == L',' && depth <= 0)
        {
            out.push_back(Trim(cur));
            cur.clear();
        }
        else
            cur += c;
    }
    out.push_back(Trim(cur));
    return out;
}

static bool IsZero(const std::wstring& a)
{
    return _wcsicmp(a.c_str(), L"NULL") == 0 || a == L"0" || _wcsicmp(a.c_str(), L"0h") == 0;
}

// "addr Name" -> true, name = "Name"
static bool IsAddr(const std::wstring& a, std::wstring& name)
{
    if (a.size() > 5 && _wcsnicmp(a.c_str(), L"addr", 4) == 0 && iswspace(a[4]))
    {
        name = Trim(a.substr(4));
        return !name.empty();
    }
    return false;
}

static std::wstring Hex(unsigned v)
{
    wchar_t buf[32];
    swprintf_s(buf, L"%X", v);
    std::wstring s = buf;
    if (iswalpha(s[0])) s = L"0" + s;   // MASM needs a leading digit
    return s + L"h";
}

// Appends A or W unless the name already ends with an uppercase A / W.
static std::wstring ApplySuffix(const std::wstring& f, int suffix)
{
    if (suffix == 0 || f.empty()) return f;
    wchar_t last = f.back();
    if (last == L'A' || last == L'W') return f;
    return f + (suffix == 1 ? L"A" : L"W");
}

// Removes a trailing ";" comment (ignoring semicolons inside quotes).
static std::wstring StripComment(const std::wstring& s)
{
    wchar_t quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        wchar_t c = s[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == L'"' || c == L'\'') { quote = c; continue; }
        if (c == L';') return s.substr(0, i);
    }
    return s;
}

// True if the (trimmed) text starts with the word "invoke".
static bool IsInvokeLine(const std::wstring& s)
{
    return s.size() >= 6 && _wcsnicmp(s.c_str(), L"invoke", 6) == 0 &&
        (s.size() == 6 || iswspace(s[6]));
}

// Parses "invoke Func,arg1,arg2,..." ; returns an error text or empty string.
static std::wstring ParseInvoke(const std::wstring& line, std::wstring& func,
    std::vector<std::wstring>& args)
{
    std::wstring s = Trim(line);

    if (!IsInvokeLine(s))
        return L"; Error: the line must start with \"invoke\".";

    s = Trim(s.substr(6));
    if (s.empty())
        return L"; Error: function name is missing.";

    std::vector<std::wstring> parts = SplitArgs(s);
    func = parts[0];
    if (func.empty())
        return L"; Error: function name is missing.";

    for (wchar_t c : func)
        if (iswspace(c))
            return L"; Error: invalid function name (is \"invoke\" typed twice?).";

    for (size_t i = 1; i < parts.size(); ++i)
    {
        if (parts[i].empty())
            return L"; Error: empty argument.";
        args.push_back(parts[i]);
    }
    return L"";
}

// ---------------------------------------------------------------- x86
static std::wstring ConvertX86(const std::wstring& func, const std::vector<std::wstring>& args,
    bool locals)
{
    std::wstring r;
    for (size_t i = args.size(); i-- > 0; )      // reverse order (stdcall)
    {
        std::wstring name;
        if (IsZero(args[i]))
            r += L"push 0h\r\n";
        else if (IsAddr(args[i], name))
        {
            if (locals) r += L"lea eax, " + name + L"\r\npush eax\r\n";
            else        r += L"push OFFSET " + name + L"\r\n";
        }
        else
            r += L"push " + args[i] + L"\r\n";
    }
    r += L"call " + func;
    return r;
}

// ---------------------------------------------------------------- x64
// Microsoft x64 calling convention:
//   args 1-4 -> RCX, RDX, R8, R9
//   args 5+  -> stack, starting at [rsp+20h] (after 32 bytes shadow space)
//   RSP must be 16-byte aligned at the call.
// x64: a 32-bit register used as an argument (e.g. the "eax" returned by a
// previous call) must be its 64-bit form, otherwise "mov r9, eax" is invalid.
static std::wstring Widen(const std::wstring& arg)
{
    std::wstring a = Trim(arg);
    std::wstring l = a;
    for (auto& c : l) c = (wchar_t)towlower(c);

    static const wchar_t* r32[] = { L"eax", L"ebx", L"ecx", L"edx", L"esi", L"edi", L"ebp", L"esp" };
    static const wchar_t* r64[] = { L"rax", L"rbx", L"rcx", L"rdx", L"rsi", L"rdi", L"rbp", L"rsp" };
    for (int i = 0; i < 8; ++i)
        if (l == r32[i]) return r64[i];

    // r8d..r15d -> r8..r15
    if (l.size() >= 3 && l[0] == L'r' && iswdigit(l[1]) && l.back() == L'd')
    {
        bool digits = true;
        for (size_t i = 1; i + 1 < l.size(); ++i)
            if (!iswdigit(l[i])) digits = false;
        if (digits) return l.substr(0, l.size() - 1);
    }
    return arg;
}

static std::wstring ConvertX64(const std::wstring& func, const std::vector<std::wstring>& argsIn,
    bool useFrame)
{
    std::vector<std::wstring> args = argsIn;
    for (auto& a : args) a = Widen(a);

    static const wchar_t* R64[4] = { L"rcx", L"rdx", L"r8",  L"r9" };
    static const wchar_t* R32[4] = { L"ecx", L"edx", L"r8d", L"r9d" };

    const size_t n = args.size();
    const size_t k = n > 4 ? n - 4 : 0;          // stack arguments
    unsigned frame = 0x20 + 8 * (unsigned)k;
    if (frame % 16 == 0) frame += 8;             // entry RSP is 8 mod 16

    std::wstring r;
    if (useFrame)
        r += L"sub rsp, " + Hex(frame) + L"\r\n";
    else
        r += L"; needs " + Hex(frame) + L" bytes of stack space\r\n";

    // Stack arguments first (they use RAX as a temporary), last one first.
    for (size_t i = n; i-- > 4; )
    {
        std::wstring dst = L"qword ptr [rsp+" + Hex(0x20 + 8 * (unsigned)(i - 4)) + L"]";
        std::wstring name;
        if (IsZero(args[i]))
            r += L"mov " + dst + L", 0\r\n";
        else if (IsAddr(args[i], name))
            r += L"lea rax, " + name + L"\r\nmov " + dst + L", rax\r\n";
        else
            r += L"mov rax, " + args[i] + L"\r\nmov " + dst + L", rax\r\n";
    }

    // Register arguments, 4th to 1st.
    for (size_t i = (n < 4 ? n : 4); i-- > 0; )
    {
        std::wstring name;
        if (IsZero(args[i]))
            r += std::wstring(L"xor ") + R32[i] + L", " + R32[i] + L"\r\n";
        else if (IsAddr(args[i], name))
            r += std::wstring(L"lea ") + R64[i] + L", " + name + L"\r\n";
        else
            r += std::wstring(L"mov ") + R64[i] + L", " + args[i] + L"\r\n";
    }

    r += L"call " + func;
    if (useFrame)
        r += L"\r\nadd rsp, " + Hex(frame);
    return r;
}

static std::wstring ConvertInvoke(const std::wstring& line, const Options& o)
{
    std::wstring func;
    std::vector<std::wstring> args;
    std::wstring err = ParseInvoke(line, func, args);
    if (!err.empty()) return err;

    func = ApplySuffix(func, o.suffix);
    return o.x64 ? ConvertX64(func, args, o.frame) : ConvertX86(func, args, o.locals);
}

// Converts a whole text: every "invoke" line is converted, everything else
// is copied through. A line ending with a comma continues on the next line
// (MASM line continuation), so multi-line invokes work too.
static std::wstring ConvertAll(const std::wstring& text, const Options& o)
{
    // Split into lines (handles \r\n and \n).
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : text)
        {
            if (c == L'\r') continue;
            if (c == L'\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
        lines.push_back(cur);
    }

    std::wstring out;
    bool prevConverted = false;     // last emitted block was a conversion
    bool prevBlank = true;          // last emitted line was empty
    bool first = true;

    auto emit = [&](const std::wstring& line)
    {
        if (!first) out += L"\r\n";
        out += line;
        first = false;
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring& raw = lines[i];
        std::wstring code = Trim(StripComment(raw));

        if (!IsInvokeLine(code))
        {
            if (code.empty() && Trim(raw).empty())
            {
                emit(L"");
                prevBlank = true;
                prevConverted = false;
                continue;
            }
            if (prevConverted && !prevBlank) emit(L"");
            emit(raw);                       // comment / other code: unchanged
            prevBlank = false;
            prevConverted = false;
            continue;
        }

        // Indentation of the first line is reused for the whole block.
        size_t ind = 0;
        while (ind < raw.size() && (raw[ind] == L' ' || raw[ind] == L'\t')) ++ind;
        std::wstring indent = raw.substr(0, ind);

        // Join continuation lines (trailing comma).
        size_t startLine = i;
        while (!code.empty() && code.back() == L',' && i + 1 < lines.size())
        {
            ++i;
            code += L" " + Trim(StripComment(lines[i]));
            code = Trim(code);
        }

        std::wstring res = ConvertInvoke(code, o);
        if (res.compare(0, 7, L"; Error") == 0)
            res = L"; Line " + std::to_wstring(startLine + 1) + L" - " + res.substr(2);

        if (prevConverted && !prevBlank) emit(L"");   // blank line between blocks
        size_t pos = 0;
        while (true)
        {
            size_t nl = res.find(L"\r\n", pos);
            std::wstring part = (nl == std::wstring::npos) ? res.substr(pos) : res.substr(pos, nl - pos);
            emit(indent + part);
            if (nl == std::wstring::npos) break;
            pos = nl + 2;
        }
        prevBlank = false;
        prevConverted = true;
    }
    return out;
}

// ---------------------------------------------------------------- reverse (asm -> invoke)
struct Ins
{
    std::wstring mn;                    // lower-case mnemonic
    std::vector<std::wstring> ops;
};

struct Pend { std::wstring raw; std::wstring code; };

static std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static Ins ParseIns(const std::wstring& code)
{
    Ins r;
    size_t i = 0;
    while (i < code.size() && !iswspace(code[i])) ++i;
    r.mn = Lower(code.substr(0, i));
    std::wstring rest = Trim(code.substr(i));
    if (!rest.empty()) r.ops = SplitArgs(rest);
    return r;
}

static bool IsReg(const std::wstring& a)
{
    static const wchar_t* regs[] = { L"eax", L"ebx", L"ecx", L"edx", L"esi", L"edi", L"ebp", L"esp",
        L"rax", L"rbx", L"rcx", L"rdx", L"rsi", L"rdi", L"rbp", L"rsp",
        L"ax", L"bx", L"cx", L"dx", L"al", L"bl", L"cl", L"dl" };
    std::wstring l = Lower(a);
    for (const wchar_t* r : regs)
        if (l == r) return true;
    return l.size() >= 2 && l[0] == L'r' && iswdigit(l[1]);      // r8..r15, r8d ...
}

static bool IsIdent(const std::wstring& a)
{
    if (a.empty() || IsReg(a) || iswdigit(a[0])) return false;
    for (wchar_t c : a)
        if (!(iswalnum(c) || c == L'_' || c == L'@' || c == L'?' || c == L'$' || c == L'.'))
            return false;
    return true;
}

// APIs whose zero argument is a number (not a NULL pointer / handle).
static bool NumericZeroFunc(const std::wstring& f)
{
    static const wchar_t* names[] = { L"ExitProcess", L"ExitThread", L"PostQuitMessage",
                                      L"Sleep", L"SetLastError" };
    for (const wchar_t* n : names)
        if (_wcsicmp(f.c_str(), n) == 0) return true;
    return false;
}

// One operand -> invoke argument: "OFFSET X" -> "addr X", zero -> NULL (or 0).
static std::wstring ArgText(const std::wstring& v, const std::wstring& func)
{
    std::wstring a = Trim(v);
    if (a.size() > 7 && _wcsnicmp(a.c_str(), L"offset", 6) == 0 && iswspace(a[6]))
        return L"addr " + Trim(a.substr(6));
    if (IsZero(a))
        return NumericZeroFunc(func) ? L"0" : L"NULL";
    return a;
}

static std::wstring MakeInvoke(const std::wstring& func, const std::vector<std::wstring>& args, int suffix)
{
    std::wstring r = L"invoke " + ApplySuffix(func, suffix);
    for (const std::wstring& a : args) r += L"," + a;
    return r;
}

// x86: pushes (and "lea reg,name / push reg") before a call -> argument list.
static bool BuildX86(const std::vector<Pend>& p, const std::wstring& func,
    std::vector<std::wstring>& args)
{
    std::vector<std::wstring> pushed;            // in push order
    bool haveLea = false;
    std::wstring leReg, leName;

    for (const Pend& it : p)
    {
        if (it.code.empty()) continue;
        Ins in = ParseIns(it.code);
        if (in.mn == L"lea" && in.ops.size() == 2)
        {
            if (haveLea) return false;
            haveLea = true;
            leReg = Lower(in.ops[0]);
            leName = in.ops[1];
        }
        else if (in.mn == L"push" && in.ops.size() == 1)
        {
            if (haveLea)
            {
                if (Lower(in.ops[0]) != leReg) return false;
                pushed.push_back(L"addr " + leName);
                haveLea = false;
            }
            else
                pushed.push_back(ArgText(in.ops[0], func));
        }
        else
            return false;
    }
    if (haveLea) return false;
    args.assign(pushed.rbegin(), pushed.rend()); // last push = first argument
    return true;
}

static int ArgRegIndex(const std::wstring& r)
{
    std::wstring l = Lower(r);
    if (l == L"rcx" || l == L"ecx") return 0;
    if (l == L"rdx" || l == L"edx") return 1;
    if (l == L"r8" || l == L"r8d") return 2;
    if (l == L"r9" || l == L"r9d") return 3;
    return -1;
}

// "qword ptr [rsp+28h]" -> argument index (4 = fifth argument), or -1.
static int StackArgIndex(const std::wstring& op)
{
    std::wstring l = Lower(op);
    size_t b = l.find(L"[rsp+");
    size_t e = l.find(L']');
    if (b == std::wstring::npos || e == std::wstring::npos || e < b + 5) return -1;
    std::wstring n = Trim(l.substr(b + 5, e - (b + 5)));
    if (n.empty()) return -1;
    unsigned long v;
    if (n.back() == L'h') v = wcstoul(n.substr(0, n.size() - 1).c_str(), nullptr, 16);
    else                  v = wcstoul(n.c_str(), nullptr, 10);
    if (v < 0x20 || (v % 8) != 0) return -1;
    return (int)((v - 0x20) / 8) + 4;
}

// Is this line part of the argument setup of an x64 call?
static bool IsArgLine64(const std::wstring& code, const std::wstring& next)
{
    Ins in = ParseIns(code);
    if (in.ops.size() != 2) return false;
    if (in.mn == L"sub") return Lower(in.ops[0]) == L"rsp";
    if (in.mn == L"xor")
        return Lower(in.ops[0]) == Lower(in.ops[1]) && ArgRegIndex(in.ops[0]) >= 0;
    if (in.mn == L"mov" || in.mn == L"lea")
    {
        if (ArgRegIndex(in.ops[0]) >= 0) return true;
        if (in.mn == L"mov" && StackArgIndex(in.ops[0]) >= 0) return true;
        if (Lower(in.ops[0]) == L"rax")          // temp for a stack argument
        {
            Ins n = ParseIns(next);
            return n.mn == L"mov" && n.ops.size() == 2 && StackArgIndex(n.ops[0]) >= 0 &&
                Lower(n.ops[1]) == L"rax";
        }
    }
    return false;
}

// x64: register / stack setup before a call -> argument list.
static bool BuildX64(const std::vector<Pend>& p, const std::wstring& func,
    std::vector<std::wstring>& args, bool& hadSub)
{
    std::vector<std::wstring> val(32);
    std::vector<bool> has(32, false);
    int maxIdx = -1;
    bool haveRax = false;
    std::wstring raxVal;
    hadSub = false;

    auto setArg = [&](int idx, const std::wstring& v) -> bool
    {
        if (idx < 0 || idx >= 32 || has[idx]) return false;
        has[idx] = true;
        val[idx] = v;
        if (idx > maxIdx) maxIdx = idx;
        return true;
    };

    for (const Pend& it : p)
    {
        if (it.code.empty()) continue;
        Ins in = ParseIns(it.code);
        if (in.mn == L"sub") { hadSub = true; continue; }
        if (in.ops.size() != 2) return false;

        const std::wstring& dst = in.ops[0];
        const std::wstring& src = in.ops[1];
        int ri = ArgRegIndex(dst);

        if (in.mn == L"xor")
        {
            if (!setArg(ri, ArgText(L"0", func))) return false;
        }
        else if (ri >= 0)
        {
            if (in.mn == L"lea") { if (!setArg(ri, L"addr " + src)) return false; }
            else if (!setArg(ri, ArgText(src, func))) return false;
        }
        else if (Lower(dst) == L"rax")
        {
            if (haveRax) return false;
            haveRax = true;
            raxVal = (in.mn == L"lea") ? L"addr " + src : ArgText(src, func);
        }
        else
        {
            int si = StackArgIndex(dst);
            if (si < 0) return false;
            std::wstring v;
            if (Lower(src) == L"rax")
            {
                if (!haveRax) return false;
                v = raxVal;
                haveRax = false;
            }
            else
                v = ArgText(src, func);
            if (!setArg(si, v)) return false;
        }
    }
    if (haveRax) return false;

    int n = maxIdx + 1;
    for (int i = 0; i < n; ++i)
        if (!has[i]) return false;
    args.assign(val.begin(), val.begin() + n);
    return true;
}

static bool IsBlank(const std::wstring& s) { return Trim(s).empty(); }

// Converts push/call (x86) or register setup/call (x64) blocks back to "invoke".
// Everything that is not part of such a block is copied through unchanged.
static std::wstring ConvertAllReverse(const std::wstring& text, const Options& o)
{
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : text)
        {
            if (c == L'\r') continue;
            if (c == L'\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
        lines.push_back(cur);
    }

    struct Entry { std::wstring text; bool conv; };
    std::vector<Entry> out;
    std::vector<Pend> pend;          // lines that may belong to the next call
    bool dropAdd = false;            // x64: skip the "add rsp" after a converted call

    auto flush = [&]()
    {
        for (const Pend& p : pend) out.push_back(Entry{ p.raw, false });
        pend.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring& raw = lines[i];
        std::wstring code = Trim(StripComment(raw));

        if (code.empty())                        // blank line or comment only
        {
            if (!pend.empty()) pend.push_back(Pend{ raw, L"" });
            else out.push_back(Entry{ raw, false });
            continue;
        }

        Ins in = ParseIns(code);

        if (dropAdd)
        {
            dropAdd = false;
            if (in.mn == L"add" && in.ops.size() == 2 && Lower(in.ops[0]) == L"rsp")
                continue;
        }

        std::wstring next = (i + 1 < lines.size()) ? Trim(StripComment(lines[i + 1])) : L"";

        bool hold = false;
        if (!o.x64)
        {
            if (in.mn == L"push" && in.ops.size() == 1)
                hold = true;
            else if (in.mn == L"lea" && in.ops.size() == 2)
            {
                Ins n = ParseIns(next);
                hold = n.mn == L"push" && n.ops.size() == 1 &&
                    Lower(n.ops[0]) == Lower(in.ops[0]);
            }
        }
        else
            hold = IsArgLine64(code, next);

        if (hold) { pend.push_back(Pend{ raw, code }); continue; }

        if (in.mn == L"call" && in.ops.size() == 1 && IsIdent(in.ops[0]))
        {
            std::vector<std::wstring> args;
            bool hadSub = false;
            bool ok = o.x64 ? BuildX64(pend, in.ops[0], args, hadSub)
                            : BuildX86(pend, in.ops[0], args);
            if (ok && (!args.empty() || hadSub))
            {
                size_t ind = 0;
                while (ind < raw.size() && (raw[ind] == L' ' || raw[ind] == L'\t')) ++ind;
                out.push_back(Entry{ raw.substr(0, ind) + MakeInvoke(in.ops[0], args, o.suffix), true });
                pend.clear();
                dropAdd = o.x64 && hadSub;
                continue;
            }
        }

        flush();
        out.push_back(Entry{ raw, false });
    }
    flush();

    // Remove blank lines between two converted invokes.
    std::vector<Entry> fin;
    for (size_t a = 0; a < out.size(); )
    {
        if (out[a].conv || !IsBlank(out[a].text)) { fin.push_back(out[a]); ++a; continue; }
        size_t b = a;
        while (b < out.size() && !out[b].conv && IsBlank(out[b].text)) ++b;
        bool between = !fin.empty() && fin.back().conv && b < out.size() && out[b].conv;
        if (!between)
            for (size_t c = a; c < b; ++c) fin.push_back(out[c]);
        a = b;
    }

    std::wstring res;
    for (size_t i = 0; i < fin.size(); ++i)
    {
        if (i) res += L"\r\n";
        res += fin[i].text;
    }
    return res;
}

// ---------------------------------------------------------------- UI logic
static bool IsChecked(HWND h)
{
    return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static Options GetOptions()
{
    Options o;
    o.x64 = IsChecked(g_hX64);
    o.reverse = IsChecked(g_hRev);
    o.locals = IsChecked(g_hLocals);
    o.frame = IsChecked(g_hFrame);
    int sel = (int)SendMessageW(g_hSuffix, CB_GETCURSEL, 0, 0);
    o.suffix = sel < 0 ? 0 : sel;
    return o;
}

// "Locals" only matters for x86, "frame" only for x64 (both: invoke -> asm only).
static void UpdateControls()
{
    bool x64 = IsChecked(g_hX64);
    bool rev = IsChecked(g_hRev);
    EnableWindow(g_hLocals, !x64 && !rev);
    EnableWindow(g_hFrame, x64 && !rev);
}

static std::wstring GetInputText()
{
    int len = GetWindowTextLengthW(g_hInput);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(g_hInput, &text[0], len + 1);
    text.resize(len);
    return text;
}

static void DoConvert()
{
    Options o = GetOptions();
    std::wstring text = GetInputText();
    std::wstring res = o.reverse ? ConvertAllReverse(text, o) : ConvertAll(text, o);
    SetWindowTextW(g_hOutput, res.c_str());
    SetWindowTextW(g_hLabel, DEFAULT_LABEL);
}

// When the direction changes and the input still holds the built-in sample,
// replace it with the sample for the new direction.
static void SwapSample()
{
    std::wstring t = GetInputText();
    if (IsChecked(g_hRev))
    {
        if (t == SAMPLE_FWD) SetWindowTextW(g_hInput, SAMPLE_REV);
    }
    else if (t == SAMPLE_REV)
        SetWindowTextW(g_hInput, SAMPLE_FWD);
}

static void Reconvert()
{
    UpdateControls();
    if (GetWindowTextLengthW(g_hOutput) > 0)
        DoConvert();
}

static void CopyOutput(HWND hWnd)
{
    int len = GetWindowTextLengthW(g_hOutput);
    if (len == 0) return;

    std::wstring text(len + 1, L'\0');
    GetWindowTextW(g_hOutput, &text[0], len + 1);

    size_t bytes = (len + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return;

    void* p = GlobalLock(h);
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(h);

    if (OpenClipboard(hWnd))
    {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h))
            SetWindowTextW(g_hLabel, L"Copied to clipboard");
        else
            GlobalFree(h);
        CloseClipboard();
    }
    else
        GlobalFree(h);
}

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_hWhite = CreateSolidBrush(RGB(255, 255, 255));
        g_hMono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Courier New");
        g_hUI = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            SAMPLE_FWD,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            8, 8, 504, 130, hWnd, (HMENU)IDC_INPUT, nullptr, nullptr);

        g_hOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            8, 146, 504, 200, hWnd, (HMENU)IDC_OUTPUT, nullptr, nullptr);

        // Row 0: direction
        g_hFwd = CreateWindowExW(0, L"BUTTON", L"invoke  ->  asm",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            12, 354, 200, 20, hWnd, (HMENU)IDC_FWD, nullptr, nullptr);

        g_hRev = CreateWindowExW(0, L"BUTTON", L"asm  ->  invoke",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            222, 354, 200, 20, hWnd, (HMENU)IDC_REV, nullptr, nullptr);

        // Row 1: architecture + API suffix
        g_hX86 = CreateWindowExW(0, L"BUTTON", L"x86 (MASM32)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            12, 380, 110, 20, hWnd, (HMENU)IDC_X86, nullptr, nullptr);

        g_hX64 = CreateWindowExW(0, L"BUTTON", L"x64 (ML64)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            130, 380, 100, 20, hWnd, (HMENU)IDC_X64, nullptr, nullptr);

        g_hSuffix = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | WS_VSCROLL | CBS_DROPDOWNLIST,
            240, 377, 272, 120, hWnd, (HMENU)IDC_SUFFIX, nullptr, nullptr);

        // Row 2: check boxes
        g_hLocals = CreateWindowExW(0, L"BUTTON", L"x86: addr of locals -> lea",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            12, 406, 200, 20, hWnd, (HMENU)IDC_LOCALS, nullptr, nullptr);

        g_hFrame = CreateWindowExW(0, L"BUTTON", L"x64: sub / add rsp",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            222, 406, 290, 20, hWnd, (HMENU)IDC_FRAME, nullptr, nullptr);

        // Row 3: buttons
        g_hCopy = CreateWindowExW(0, L"BUTTON", L"Co&py",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            252, 434, 126, 26, hWnd, (HMENU)IDC_COPY, nullptr, nullptr);

        g_hButton = CreateWindowExW(0, L"BUTTON", L"&Convert (Ctrl+Enter)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            386, 434, 126, 26, hWnd, (HMENU)IDOK, nullptr, nullptr);

        g_hLabel = CreateWindowExW(0, L"STATIC", DEFAULT_LABEL,
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 470, 520, 16, hWnd, (HMENU)IDC_LABEL, nullptr, nullptr);

        SendMessageW(g_hSuffix, CB_ADDSTRING, 0, (LPARAM)L"API name: as is");
        SendMessageW(g_hSuffix, CB_ADDSTRING, 0, (LPARAM)L"API name + A (ANSI)");
        SendMessageW(g_hSuffix, CB_ADDSTRING, 0, (LPARAM)L"API name + W (Unicode)");
        SendMessageW(g_hSuffix, CB_SETCURSEL, 0, 0);

        SendMessageW(g_hFwd, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_hX86, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_hFrame, BM_SETCHECK, BST_CHECKED, 0);

        HWND ui[] = { g_hFwd, g_hRev, g_hX86, g_hX64, g_hSuffix, g_hLocals, g_hFrame,
                      g_hCopy, g_hButton, g_hLabel };
        for (HWND h : ui)
            SendMessageW(h, WM_SETFONT, (WPARAM)g_hUI, TRUE);
        SendMessageW(g_hInput, WM_SETFONT, (WPARAM)g_hMono, TRUE);
        SendMessageW(g_hOutput, WM_SETFONT, (WPARAM)g_hMono, TRUE);

        UpdateControls();
        SetFocus(g_hInput);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDOK:
            DoConvert();
            return 0;

        case IDC_COPY:
            CopyOutput(hWnd);
            return 0;

        case IDC_FWD:
        case IDC_REV:
            if (HIWORD(wp) == BN_CLICKED)
            {
                SwapSample();
                Reconvert();
            }
            return 0;

        case IDC_X86:
        case IDC_X64:
        case IDC_LOCALS:
        case IDC_FRAME:
            if (HIWORD(wp) == BN_CLICKED)
                Reconvert();
            return 0;

        case IDC_SUFFIX:
            if (HIWORD(wp) == CBN_SELCHANGE)
                Reconvert();
            return 0;
        }
        break;

        // Blue text for the input box, green for the output box, white background.
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wp;
        HWND hCtl = (HWND)lp;
        if (hCtl == g_hInput)
        {
            SetTextColor(hdc, RGB(0, 0, 255));
            SetBkColor(hdc, RGB(255, 255, 255));
            return (LRESULT)g_hWhite;
        }
        if (hCtl == g_hOutput)
        {
            SetTextColor(hdc, RGB(0, 190, 0));
            SetBkColor(hdc, RGB(255, 255, 255));
            return (LRESULT)g_hWhite;
        }
        if (hCtl == g_hLabel)
        {
            SetTextColor(hdc, RGB(90, 90, 90));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_DESTROY:
        DeleteObject(g_hMono);
        DeleteObject(g_hWhite);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"InvokeConverterWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, 520, 492 };               // client area size
    AdjustWindowRect(&rc, style, FALSE);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    // Center the window on the screen's work area.
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"Invoke Converter", style,
        x, y, w, h, nullptr, nullptr, hInst, nullptr);

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (m.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (m.wParam == VK_RETURN)          // Ctrl+Enter = convert
            {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                continue;
            }
            if (m.wParam == 'A' && (m.hwnd == g_hInput || m.hwnd == g_hOutput))
            {
                SendMessageW(m.hwnd, EM_SETSEL, 0, -1);   // Ctrl+A = select all
                continue;
            }
        }
        if (!IsDialogMessageW(hWnd, &m))        // Tab / Alt+C support
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}