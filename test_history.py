#!/usr/bin/python
#
# bg_test: tests the bg command
# 
# Test the stop command for stopping a process by its pid.
# Requires the following commands to be implemented
# or otherwise usable:
#
#	Tests the history custom builtin
#

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *


setup_tests()

# Tests history command
sendline("ls")
sendline("cd ")
sendline("pwd")
sendline("history")
expect("1 ls")
expect("2 cd")
expect("3 pwd")
expect("4 history")

# test event discriptors
sendline("echo hi")
sendline("!!")
expect("hi")

sendline("!3")
expect("home")

sendline("echo hello")
sendline("!-1")
expect("hello")

sendline("!echo")
expect("hello")

sendline("^hello^go away")
expect("go away")

sendline("echo sup; !#")
sendline("!#")
expect("sup")
expect("sup")

# Test invalid cases
sendline("!40")
expect("no such file or directory")

sendline("!fg")
expect("no such file or directory")

test_success()