#!/usr/bin/python

'''
Convert hex string into binary representation
String should have only hexadecimal characters, with new line stripped
'''

import sys
import fileinput
import re
import struct

line = fileinput.input()[0]

while len(line) > 0:
  x = int(line[:2], 16)
  line = line[2:]
  s = struct.pack("<B", x)
  sys.stdout.write(s)

