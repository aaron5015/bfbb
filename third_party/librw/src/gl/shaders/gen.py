# What the Makefile's sed recipe does, for machines without make or sed.
#
#     python gen.py header.frag header_frag_src header_fs.inc
#
# Byte-for-byte the same output: a blank line passes through as a blank line,
# because sed's `..*` does not match one.
#
# **A double quote in the shader is refused rather than emitted.** Every line
# becomes a C string literal and nothing escapes anything, so one quote ends the
# literal early and the error surfaces hundreds of lines away in a generated
# file, naming a column in machine-written code. Escaping it here instead would
# make this disagree with the Makefile, and the next person to run make would
# get the broken version back.
import io, sys

name, var, out = sys.argv[1], sys.argv[2], sys.argv[3]
src = io.open(name, encoding='utf-8', newline='').read().replace('\r\n', '\n')

if src.endswith('\n'):
    src = src[:-1]

lines = ['const char *' + var + ' =']
for n, line in enumerate(src.split('\n'), 1):
    if '"' in line:
        sys.exit('%s:%d: a double quote cannot survive becoming a string '
                 'literal -- reword the line\n  %s' % (name, n, line.strip()))
    lines.append('"' + line + '\\n"' if line else '')
lines.append(';')

io.open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print('wrote ' + out)
