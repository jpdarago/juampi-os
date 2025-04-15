#!/usr/bin/python
with open('archs.txt','r') as f:
	print "const char * archs = {" + ",\n".join(map(lambda p: '"%s"' % p.rstrip(),f.readlines())) + "}"
