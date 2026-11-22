import sys
shell, js, out = sys.argv[1:4]
s = open(shell).read(); j = open(js).read()
open(out, 'w').write(s.replace('<!--SYRUP_JS-->', '<script>\n' + j.replace('</script>', '<\\/script>') + '\n</script>'))
