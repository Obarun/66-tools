import sys
name, src = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
out = sys.stdout
out.write('unsigned char %s[] = {\n' % name)
for i in range(0, len(data), 12):
    out.write('  ' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',\n')
out.write('};\nunsigned int %s_len = %d;\n' % (name, len(data)))
