#!/usr/bin/env python3
"""Regenerate src/formats/script_funcs.cpp from the original joan.exe + shipped scripts.

usage: gen_funcs.py <game_dir> > src/formats/script_funcs.cpp      (needs objdump)

Evidence used (GOG joan.exe, PE timestamp 2004-01-19):
  * table at VA 0xa1a250: 407 x {u32 id; char name[52]; u32 return_type}; lookup FUN_00661e10
    loops i < 0x197, _stricmp(name), returns id, writes return_type.
  * FUNC executor FUN_00649920: switch(id - 1) over 0x197 cases, jump table at VA 0x64eca3.
    Argument N is fetched by `push N; mov ecx,[ebp-0x10]; call 0x401c08`; arity = max N.
    `fld [eax+0x10]` after the fetch marks a float argument.
  * ids whose case is the default (VA 0x64ec90) are DIALOG (316-322) / ACTION (323,324,326,327)
    functions; their arities come from the DIALOG/ACTION executors (VA 0x65fd70..0x661300).
  * argument types not visible in the executor (int vs string pointer) are taken from literal
    arguments in script/scenario/*.sce; '?' when no shipped script passes a literal there.
"""
import collections, glob, os, re, struct, subprocess, sys

IMAGE = 0x400000  # file offset == RVA for joan.exe .text/.data
TABLE, COUNT, JT, DEFAULT = 0xa1a254, 407, 0x64eca3, 0x64ec90
SW_LO, SW_HI = 0x649920, 0x64ec90
DLG_LO, DLG_HI = 0x65fd70, 0x661300


def main(game):
    exe = open(os.path.join(game, 'joan.exe'), 'rb').read()
    u32 = lambda va: struct.unpack_from('<I', exe, va - IMAGE)[0]
    funcs = {}
    for i in range(COUNT):
        o = TABLE + 60 * i
        name = exe[o - IMAGE:o - IMAGE + 52].split(b'\0')[0].decode()
        funcs[u32(o - 4)] = (name, u32(o + 52))
    targets = [u32(JT + 4 * i) for i in range(COUNT)]

    asm = []
    dis = subprocess.run(['objdump', '-d', '-M', 'intel', f'--start-address={SW_LO:#x}', f'--stop-address={DLG_HI:#x}',
                          os.path.join(game, 'joan.exe')], capture_output=True, text=True, check=True).stdout
    for line in dis.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\t[0-9a-f ]+\t(.*)', line)
        if m:
            asm.append((int(m.group(1), 16), m.group(2).strip()))
    index = {a: i for i, (a, _) in enumerate(asm)}
    case_set = set(targets)

    def arg_at(i):
        for back in (1, 2):
            m = re.match(r'push\s+0x([0-9a-f]+)$', asm[i - back][1])
            if m:
                return int(m.group(1), 16)
        return None

    def case_args(start):
        seen, stack, args = set(), [start], {}
        while stack:
            a = stack.pop()
            i = index.get(a)
            while i is not None and i < len(asm):
                a, ins = asm[i]
                if a in seen or (a != start and a in case_set):
                    break
                seen.add(a)
                if ins == 'call   0x401c08':
                    n = arg_at(i)
                    nxt = asm[i + 1][1]
                    kind = 'x'
                    if '[eax+0x10]' in nxt:
                        if nxt.startswith('fld'):
                            kind = 'f'
                        elif re.search(r'\b(cx|dx|ax|bx|cl|dl|al|bl),(WORD|BYTE)', nxt) or nxt.startswith('movsx'):
                            kind = 'n'  # truncated to 8/16 bits: integer
                    args.setdefault(n, set()).add(kind)
                m = re.match(r'(j\w+)\s+0x([0-9a-f]+)$', ins)
                if m:
                    dst = int(m.group(2), 16)
                    if dst != DEFAULT and SW_LO <= dst < SW_HI:
                        stack.append(dst)
                    if m.group(1) == 'jmp':
                        break
                if ins.startswith('ret'):
                    break
                i += 1
        return args

    special = collections.defaultdict(int)  # DIALOG/ACTION executors: segments start at `cmp reg, id`
    cur = None
    for i, (a, ins) in enumerate(asm):
        if not DLG_LO <= a < DLG_HI:
            continue
        m = re.match(r'cmp\s+e[a-d]x,0x(1[34][0-9a-f])$', ins)
        if m:
            cur = int(m.group(1), 16)
            special[cur] += 0
        elif ins == 'call   0x401c08' and cur and arg_at(i):
            special[cur] = max(special[cur], arg_at(i))

    literal = collections.defaultdict(lambda: collections.defaultdict(set))
    for path in glob.glob(os.path.join(game, 'script', 'scenario', '*.sce')):
        for kw, args in calls(open(path, 'rb').read().decode('latin-1')):
            for p, a in enumerate(args[1:], 1):
                literal[args[0].strip('"')][p].add('s' if a.startswith('"') else 'f' if re.fullmatch(r'-?\d*\.\d*', a) else 'i')

    out = ['// Generated from joan.exe (GOG build, PE timestamp 2004-01-19) by tools/joa-script/gen_funcs.py.',
           '// See docs/formats/SCRIPTS_AND_GAME_DATA.md "Function table" for provenance and confidence.',
           '#include "formats/script_funcs.hpp"', '', '#include "formats/vfs_fs.hpp"', '',
           'namespace joa::formats {', '', 'namespace {', 'constexpr ScriptFunc kFuncs[] = {']
    rtype = {1: 'Int', 2: 'Float', 3: 'String'}
    for fid in range(1, COUNT + 1):
        name, rt = funcs[fid]
        tg = targets[fid - 1]
        if tg == DEFAULT:
            args, arity = {}, special[fid]
            ex = 'Dialog' if 316 <= fid <= 322 else 'Action'
            tg = 0
        else:
            args = case_args(tg)
            arity = max((k for k in args if k), default=0)
            ex = 'Func'
        sig = ''
        for p in range(1, arity + 1):
            lit = literal[name][p] if name in literal else set()
            if 'f' in args.get(p, set()) or 'f' in lit:
                sig += 'f'
            elif lit == {'s'}:
                sig += 's'
            elif lit and 's' not in lit:
                sig += 'i'
            elif not lit and 'n' in args.get(p, set()):
                sig += 'i'
            else:
                sig += '?'
        if name == 'format_string':  # variadic: format, then any number of values
            sig = 's?'
        out.append(f'    {{{fid}, "{name}", ScriptType::{rtype[rt]}, {arity}, "{sig}", 0x{tg:06x}, ScriptExecutor::{ex}}},')
    out += ['};', '}  // namespace', '', 'std::span<const ScriptFunc> script_funcs() { return kFuncs; }', '',
            'const ScriptFunc* find_script_func(std::string_view name) {', '    for (const auto& f : kFuncs)',
            '        if (iequals(f.name, name)) return &f;', '    return nullptr;', '}', '', '}  // namespace joa::formats', '']
    sys.stdout.write('\n'.join(out))


def calls(src):
    """Yield (keyword, [name, arg...]) for FUNC/ACTION/DIALOG calls, '//' comments removed."""
    src = '\n'.join(strip_comment(l) for l in src.replace('\r', '').split('\n'))
    for m in re.finditer(r'\b(FUNC|ACTION|DIALOG)\s*\(', src):
        i, depth, quoted, cur, args = m.end(), 1, False, '', []
        while depth:
            c = src[i]
            if c == '"':
                quoted = not quoted
            if not quoted and c in '(),':
                if c == '(':
                    depth += 1
                elif c == ')':
                    depth -= 1
                elif depth == 1:
                    args.append(cur.strip())
                    cur, i = '', i + 1
                    continue
            if depth:
                cur += c
            i += 1
        args.append(cur.strip())
        yield m.group(1), args


def strip_comment(line):
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif not quoted and line.startswith('//', i):
            return line[:i]
    return line


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
