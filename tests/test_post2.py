# $Id: test_post2.py,v 1.5 2002/08/12 13:31:31 kjetilja Exp $

import pycurl

pf = ['field1=this is a test using httppost & stuff', 'field2=value2']

print "\n** INFO: For some reason this script does not work any longer, use the"
print "** scheme in test_post.py for posting instead!\n\n"

c = pycurl.Curl()
c.setopt(c.URL, 'http://pycurl.sourceforge.net/tests/testpostvars.php')
c.setopt(c.POST, 1)
c.setopt(c.HTTPPOST, pf)
c.setopt(c.VERBOSE, 1)
c.perform()
c.close()
