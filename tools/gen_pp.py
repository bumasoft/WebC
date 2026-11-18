#!/usr/bin/env python3
"""Regenerate include/webc/pp.h (the WC_MAP machinery). Framework-internal: it
emits a committed header, never code into a user's project."""
N = 64
out = ["// WebC — generated MAP machinery (comma and semicolon forms), 1..%d slots." % N,
       "// Generated once by tools/gen_pp.py; no user project ever regenerates it.",
       "//",
       "// The slot count is the widest a `WEBC_MODEL` can be. At 32 a 33-field model reported",
       "// \"pasting formed 'WC_MAP_S_('\" — an error about the preprocessor, explained",
       "// (confidently, wrongly) as a problem with the field. A row this wide is usually two",
       "// tables: long prose belongs in its own row and its own screen.",
       "#ifndef WEBC_PP_H", "#define WEBC_PP_H", "",
       "#ifndef WC_CAT", "#define WC_CAT(a,b) WC_CAT_(a,b)", "#define WC_CAT_(a,b) a##b", "#endif",
       "#ifndef WC_ARRLEN",
       "#define WC_ARRLEN(a_) ((i32)(sizeof(a_) / sizeof((a_)[0])))",
       "#endif",
       "#ifndef WC_NARG",
       "#define WC_NARG(...) WC_NARG_(__VA_ARGS__%s)" % "".join(", %d" % i for i in range(N, 0, -1)),
       "#define WC_NARG_(%s, N, ...) N" % ", ".join("_%d" % i for i in range(1, N+1)),
       "#endif", ""]
for i in range(1, N+1):
    args = ", ".join("_%d" % j for j in range(1, i+1))
    if i == 1:
        out.append("#define WC_MAP_1(f, a) f(a)")
        out.append("#define WC_MAP_S_1(f, a) f(a)")
    else:
        out.append("#define WC_MAP_%d(f, %s) f(_1), WC_MAP_%d(f, %s)" % (i, args, i-1, ", ".join("_%d" % j for j in range(2, i+1))))
        out.append("#define WC_MAP_S_%d(f, %s) f(_1) WC_MAP_S_%d(f, %s)" % (i, args, i-1, ", ".join("_%d" % j for j in range(2, i+1))))
out += ["", "#define WC_MAP(f, ...) WC_CAT(WC_MAP_, WC_NARG(__VA_ARGS__))(f, __VA_ARGS__)",
        "#define WC_MAP_S(f, ...) WC_CAT(WC_MAP_S_, WC_NARG(__VA_ARGS__))(f, __VA_ARGS__)", "", "#endif // WEBC_PP_H"]
open('include/webc/pp.h', 'w').write("\n".join(out) + "\n")
