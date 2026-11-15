# -*- coding: utf-8 -*-
# Tạo probe DLL cho WP 8.0: mỗi DLL hệ thống mà RuffleWP80Native.dll import -> một probe_<n>.dll import ĐÚNG các hàm đó.
# Trên máy, LoadPackagedLibrary(probe) lỗi 126 = DLL hệ thống không có, 127 = thiếu hàm.
# Dùng: python make-probes.py <imports.txt của dumpbin /imports> <thư mục ra>
import io, os, re, sys

src, out = sys.argv[1], sys.argv[2]
if not os.path.isdir(out):
    os.makedirs(out)
imports = {}
cur = None
for line in io.open(src, encoding='ascii', errors='replace'):
    m = re.match(r'^ {4}(\S+\.(?:dll|DLL))\s*$', line)
    if m:
        cur = m.group(1); imports.setdefault(cur, []); continue
    m = re.match(r'^ +[0-9A-F]+ +([A-Za-z_][A-Za-z0-9_@?$]*)\s*$', line)
    if m and cur:
        imports[cur].append(m.group(1))

lines = []
for i, (dll, funcs) in enumerate(sorted(imports.items())):
    funcs = [f for f in funcs if not f.startswith('?')]  # bỏ tên C++ mangled (MSVCP/vccorlib): chỉ kiểm tra có DLL
    if not funcs:
        funcs = []
    base = 'probe_%02d' % i
    with io.open(os.path.join(out, base + '_imp.def'), 'w', newline='\r\n') as f:
        f.write(u'LIBRARY %s\nEXPORTS\n' % dll)
        for fn in funcs:
            f.write(u'    %s\n' % fn)
    with io.open(os.path.join(out, base + '.c'), 'w', newline='\r\n') as f:
        for fn in funcs:
            f.write(u'extern void %s(void);\n' % fn)
        f.write(u'__declspec(dllexport) void* probe_refs[] = {\n')
        for fn in funcs:
            f.write(u'    (void*)&%s,\n' % fn)
        f.write(u'    0 };\n')
    lines.append(u'%s %s %d' % (base, dll, len(funcs)))
with io.open(os.path.join(out, 'probes.txt'), 'w', newline='\r\n') as f:
    f.write(u'\n'.join(lines) + u'\n')
print(u'\n'.join(lines))
