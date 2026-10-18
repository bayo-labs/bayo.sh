# Pastes the translation table from tools/i18n.py into src/bayosh_page.html (between the I18N markers).
import json, os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
table = subprocess.check_output([sys.executable, os.path.join(here, "i18n.py")]).decode()
p = os.path.join(here, "..", "src", "bayosh_page.html")
s = open(p, encoding="utf-8").read()
s, n = re.subn(r"/\*I18N\*/.*?/\*/I18N\*/", lambda m: "/*I18N*/" + table + "/*/I18N*/", s, flags=re.S)
assert n == 1, "I18N markers not found"
open(p, "w", encoding="utf-8").write(s)
print("I18N table:", len(table), "bytes")
