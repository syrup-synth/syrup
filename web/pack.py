import sys, os, json
shell, js, out = sys.argv[1:4]
s = open(shell).read(); j = open(js).read()
rem = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'remote.html')).read()   # the phone page, for PHONE (a phone linked to this page)
s = s.replace('<!--REMOTE_HTML-->', '<script>var REMOTE_HTML = ' + json.dumps(rem).replace('</', '<\\/') + ';</script>')
open(out, 'w').write(s.replace('<!--SYRUP_JS-->', '<script>\n' + j.replace('</script>', '<\\/script>') + '\n</script>'))
