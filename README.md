# Invoke Converter

A tiny native Win32 GUI tool that converts MASM `invoke` statements into plain `push`/`call` (x86) or register-based (x64) assembly — **and back again**.

If you write MASM32 or ML64 code and need the "raw" instruction sequence behind an `invoke` (for a different assembler, for shellcode/position-independent code, for learning the calling conventions, or just to see what the macro expands to), this tool does it in one click.

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B-orange)
![License](https://img.shields.io/badge/license-MIT-green)
![Toolchain](https://img.shields.io/badge/Visual%20Studio-2022%20(v143)-purple)

---

## Features

- **Two-way conversion**
  - `invoke` → assembly
  - assembly → `invoke` (reverse mode)
- **x86 (MASM32)** output using the `stdcall` convention: arguments pushed right-to-left, followed by `call`.
- **x64 (ML64)** output using the Microsoft x64 calling convention:
  - Arguments 1–4 go in `RCX`, `RDX`, `R8`, `R9`
  - Arguments 5+ go on the stack, starting at `[rsp+20h]` (after the 32-byte shadow space)
  - Stack-frame size is computed so `RSP` is 16-byte aligned at the call
- **Multi-line input** – every `invoke` line is converted; comments, labels, other instructions and blank lines pass through unchanged.
- **Line continuation** – an `invoke` line ending in a comma continues on the next line, just like in MASM.
- **API name suffix** – leave the name as is, or append `A` (ANSI) / `W` (Unicode) automatically. A name that already ends in an uppercase `A` or `W` is left alone.
- **`addr` handling**
  - x86: `addr Name` → `push OFFSET Name`, or `lea eax, Name` + `push eax` for local variables
  - x64: `addr Name` → `lea <reg>, Name`
- **Zero optimization** – `NULL`, `0`, and `0h` become `push 0h` (x86) or `xor ecx, ecx` / `mov qword ptr [rsp+..], 0` (x64).
- **Register widening (x64)** – a 32-bit register used as an argument (e.g. `eax` from a previous call) is automatically widened to its 64-bit form so `mov r9, eax` is never emitted.
- **Smart reverse mode** – recognizes `OFFSET X` → `addr X`, and knows that zero is a numeric `0` (not `NULL`) for APIs such as `ExitProcess`, `ExitThread`, `PostQuitMessage`, `Sleep` and `SetLastError`.
- **Friendly error messages** – malformed lines produce a comment such as `; Line 3 - Error: function name is missing.` instead of failing silently.
- **Copy button** – one click to copy the result to the clipboard.
- **Keyboard shortcuts** – `Ctrl+Enter` converts, `Ctrl+A` selects all text.
- No dependencies beyond the Windows API; a single `main.cpp`.

---

## Examples

### `invoke` → x86

**Input**
```asm
invoke MessageBox,NULL,addr MsgText,NULL,NULL
invoke ExitProcess,0
```

**Output**
```asm
push 0h
push 0h
push OFFSET MsgText
push 0h
call MessageBox

push 0h
call ExitProcess
```

### `invoke` → x64

**Input**
```asm
invoke MessageBox,NULL,addr MsgText,NULL,NULL
```

**Output** (with *sub / add rsp* enabled)
```asm
sub rsp, 28h
xor r9d, r9d
xor r8d, r8d
lea rdx, MsgText
xor ecx, ecx
call MessageBox
add rsp, 28h
```

### Assembly → `invoke` (reverse mode)

Paste a `push`/`call` block (x86) or a register-setup + `call` block (x64) and switch the direction to **asm → invoke** to get the original `invoke` line back.

---

## Usage

1. Launch the application.
2. Choose a direction: **invoke → asm** or **asm → invoke**.
3. Choose the target: **x86 (MASM32)** or **x64 (ML64)**.
4. Paste or type your code into the top box.
5. Press **Convert** (or `Ctrl+Enter`).
6. Click **Copy** to put the result on the clipboard.

### Options

| Option | Applies to | Description |
|---|---|---|
| `x86 (MASM32)` / `x64 (ML64)` | both | Target architecture and calling convention |
| `invoke -> asm` / `asm -> invoke` | both | Conversion direction |
| API name: as is / + A / + W | invoke → asm | Append an ANSI or Unicode suffix to the API name |
| x86: addr of locals -> lea | x86, invoke → asm | Emit `lea eax, Name` + `push eax` instead of `push OFFSET Name` (needed for local variables) |
| x64: sub / add rsp | x64, invoke → asm | Emit the `sub rsp, N` / `add rsp, N` frame around the call. When off, a comment states how much stack space is needed |

> **Note:** `addr` of a *local* variable cannot be expressed with `push OFFSET`, which is why the "locals" option exists. Enable it whenever the argument refers to a stack variable.

---

## Building

### Requirements

- Windows
- Visual Studio 2022 (platform toolset **v143**) with the *Desktop development with C++* workload

### Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/rzavx/asmconverter.git
   ```
2. Open `asmconverter.sln` in Visual Studio 2022.
3. Select a configuration (**Release** recommended) and a platform (**x86** or **x64**).
4. Build the solution (`Ctrl+Shift+B`).

The resulting executable is a standard GUI (`/SUBSYSTEM:WINDOWS`) application using the wide-character entry point `wWinMain`, with the Common Controls v6 manifest dependency embedded via `#pragma comment`.

---

## Project Structure

```
asmconverter/
├── asmconverter.sln
└── asmconverter/
    ├── asmconverter.vcxproj
    ├── asmconverter.vcxproj.filters
    ├── asmconverter.vcxproj.user
    └── main.cpp          # entire application (parser, converters, GUI)
```

---

## How It Works

- **Parsing** – the line is split on commas that are *not* inside quotes, `[]`, `()` or `<>`, so arguments like `"Hello, world"` or `[ebp+8]` stay intact. Trailing `;` comments are ignored (unless inside quotes).
- **x86 converter** – walks the arguments in reverse and emits `push` instructions, then `call`.
- **x64 converter** – fills the stack arguments first (using `RAX` as a temporary), then the register arguments from the 4th down to the 1st, and finally emits the call.
- **Reverse converter** – scans the text for blocks of argument setup immediately preceding a `call` and folds them back into a single `invoke` line; everything else is copied unchanged.

---

## Limitations

- Only the common MASM `invoke` syntax is handled; advanced features such as `ADDR` expressions inside complex operands, `vararg` handling, or type-checking against `PROTO` declarations are not performed.
- Reverse mode is heuristic: it works best on code that follows the exact patterns this tool generates.
- `lea eax` / `rax` is used as a scratch register for `addr` of locals and for stack arguments in x64 — make sure it is free at that point in your code.
- The tool performs a textual transformation only; it does not assemble or validate the result.

---

## Contributing

Issues and pull requests are welcome. If you find a pattern that converts incorrectly, please open an issue with the input, the options you used, and the expected output.

---

## License

This project is licensed under the [MIT License](LICENSE).
